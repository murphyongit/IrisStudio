// (built without the precompiled header, which has imgui.h: see ReShade.hpp)
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string_view>

#include <Windows.h>

// (by its path: Windows finds this folder's ReShade.hpp for "reshade.hpp")
#include "../third-party/reshade/include/reshade.hpp"

#include "Logger.hpp"
#include "ReShade.hpp"

namespace IWXMVM::ReShade
{
    using reshade::api::effect_runtime;
    using reshade::api::effect_technique;
    using reshade::api::effect_uniform_variable;
    using reshade::api::format;

    namespace
    {
        HMODULE module = nullptr;
        std::atomic<Status> status = Status::NotFound;

        // what other threads read and ask for
        std::mutex mutex;
        std::shared_ptr<const Effects> effects;
        Values values;
        std::vector<std::pair<std::string, Value>> presetValuesAsked;
        std::vector<std::pair<std::string, bool>> presetTechniquesAsked;
        std::vector<MenuChange> menuChanges;
        struct AsMenu
        {
            std::string id;
            Value value{};
            bool save = false;
        };
        std::vector<AsMenu> asMenuAsked;
        FrameListener listener;
        std::atomic<std::uint32_t> presetRestores = 0;

        // the render thread's
        effect_runtime* runtime = nullptr;
        bool readAgain = true;
        // ReShade switched to another preset: what keys hold doesn't go through the next read, whose preset's own values
        // are the new preset's
        bool presetChanged = false;
        struct UniformState
        {
            effect_uniform_variable handle{};
            format type = format::r32_float;
            std::optional<Value> held;      // this frame's, from the listener
            std::optional<Value> wasHeld;   // the frame before's
            Value preset{};                 // the preset's own while held
        };
        struct TechniqueState
        {
            effect_technique handle{};
            std::optional<bool> held;
            std::optional<bool> wasHeld;
            bool preset = false;
        };
        std::vector<UniformState> uniforms;
        std::vector<TechniqueState> techniques;
        std::unordered_map<std::uint64_t, std::size_t> uniformByHandle;
        std::unordered_map<std::uint64_t, std::size_t> techniqueByHandle;
        enum class ClockSource
        {
            Timer,
            FrameTime,
            FrameCount,
        };
        std::vector<std::pair<UniformState, ClockSource>> clockUniforms;
        std::optional<Clock> clock;
        // set while the mod sets a value itself, so its own changes aren't taken for the menu's
        bool settingOurs = false;
        // ReShade's menu changed something while keys held values: ReShade saved the preset with them (Q1)
        bool restorePreset = false;
        // a save asked for (SavePreset), and whether ReShade saves after its menu's changes
        std::atomic<bool> saveAsked = false;
        bool autoSave = false;
        // the preset file as the mod last saw it, and when the mod saved it last (ReShade writes the file from its
        // cache later, up to a second or so)
        std::filesystem::file_time_type presetWritten{};
        std::uint64_t frame = 0;
        std::chrono::steady_clock::time_point savedAt{};

        // the frames ReShade drew its effects in (see GetReload), and whether they're on (its effects' toggle key
        // turns them all off)
        std::atomic<std::uint32_t> effectFrames = 0;
        std::atomic<bool> effectsOn = true;
        std::atomic<std::uint32_t> effectFramesAtReset = 0;
        std::atomic<bool> drewEffects = false;
        // (the harness's) its effects' toggle asked for: -1 none
        std::atomic<int> effectsAsked = -1;
        // whether it's loading its effects (see OnReloadedEffects), and the times it said so in this load
        std::atomic<bool> loading = false;
        std::uint32_t loadEvents = 0;
        // (the harness's) taken as loading until then, GetTickCount64
        std::atomic<std::uint64_t> loadingUntil = 0;
        // ReShade.ini's NoReloadOnInit: it doesn't load them after a reset
        bool noReloadOnInit = false;

        std::string UniformAnnotation(effect_uniform_variable variable, const char* name)
        {
            char value[1024] = "";
            std::size_t size = sizeof(value);
            if (!runtime->get_annotation_string_from_uniform_variable(variable, name, value, &size))
                return {};
            return value;
        }

        std::string TechniqueAnnotation(effect_technique technique, const char* name)
        {
            char value[1024] = "";
            std::size_t size = sizeof(value);
            if (!runtime->get_annotation_string_from_technique(technique, name, value, &size))
                return {};
            return value;
        }

        // ui_items: the items one after the other, each ending with a 0
        std::vector<std::string> UniformItems(effect_uniform_variable variable)
        {
            char value[4096] = "";
            std::size_t size = sizeof(value);
            if (!runtime->get_annotation_string_from_uniform_variable(variable, "ui_items", value, &size))
                return {};
            size = std::min(size, sizeof(value));
            std::vector<std::string> items;
            std::size_t start = 0;
            for (std::size_t i = 0; i < size; ++i)
            {
                if (value[i] != '\0')
                    continue;
                if (i > start)
                    items.emplace_back(value + start, i - start);
                start = i + 1;
            }
            if (start < size && value[start] != '\0')
                items.emplace_back(value + start, std::strlen(value + start));
            return items;
        }

        std::optional<float> UniformNumber(effect_uniform_variable variable, const char* name)
        {
            float number = 0.0f;
            if (runtime->get_annotation_float_from_uniform_variable(variable, name, &number, 1))
                return number;
            std::int32_t whole = 0;
            if (runtime->get_annotation_int_from_uniform_variable(variable, name, &whole, 1))
                return static_cast<float>(whole);
            return std::nullopt;
        }

        bool UniformFlag(effect_uniform_variable variable, const char* name)
        {
            bool flag = false;
            if (runtime->get_annotation_bool_from_uniform_variable(variable, name, &flag, 1))
                return flag;
            std::int32_t whole = 0;
            return runtime->get_annotation_int_from_uniform_variable(variable, name, &whole, 1) && whole != 0;
        }

        Value GetUniform(const UniformState& uniform, std::uint32_t components)
        {
            Value value{};
            switch (uniform.type)
            {
                case format::r32_float:
                    runtime->get_uniform_value_float(uniform.handle, value.data(), components);
                    break;
                case format::r32_sint:
                {
                    std::int32_t whole[4]{};
                    runtime->get_uniform_value_int(uniform.handle, whole, components);
                    for (std::uint32_t i = 0; i < components; ++i)
                        value[i] = static_cast<float>(whole[i]);
                    break;
                }
                case format::r32_uint:
                {
                    std::uint32_t whole[4]{};
                    runtime->get_uniform_value_uint(uniform.handle, whole, components);
                    for (std::uint32_t i = 0; i < components; ++i)
                        value[i] = static_cast<float>(whole[i]);
                    break;
                }
                default:
                {
                    bool flags[4]{};
                    runtime->get_uniform_value_bool(uniform.handle, flags, components);
                    for (std::uint32_t i = 0; i < components; ++i)
                        value[i] = flags[i] ? 1.0f : 0.0f;
                    break;
                }
            }
            return value;
        }

        // (ours: the mod's own change, which its events don't take for the menu's)
        void SetUniform(const UniformState& uniform, std::uint32_t components, const Value& value, bool ours = true)
        {
            settingOurs = ours;
            switch (uniform.type)
            {
                case format::r32_float:
                    runtime->set_uniform_value_float(uniform.handle, value.data(), components);
                    break;
                case format::r32_sint:
                {
                    std::int32_t whole[4]{};
                    for (std::uint32_t i = 0; i < components; ++i)
                        whole[i] = static_cast<std::int32_t>(std::lround(value[i]));
                    runtime->set_uniform_value_int(uniform.handle, whole, components);
                    break;
                }
                case format::r32_uint:
                {
                    std::uint32_t whole[4]{};
                    for (std::uint32_t i = 0; i < components; ++i)
                        whole[i] = static_cast<std::uint32_t>(std::max(0l, std::lround(value[i])));
                    runtime->set_uniform_value_uint(uniform.handle, whole, components);
                    break;
                }
                default:
                {
                    bool flags[4]{};
                    for (std::uint32_t i = 0; i < components; ++i)
                        flags[i] = value[i] >= 0.5f;
                    runtime->set_uniform_value_bool(uniform.handle, flags, components);
                    break;
                }
            }
            settingOurs = false;
        }

        void SetTechnique(const TechniqueState& technique, bool on)
        {
            settingOurs = true;
            runtime->set_technique_state(technique.handle, on);
            settingOurs = false;
        }

        // (ReShade's paths are UTF-8)
        std::filesystem::path ToPath(const std::string& path)
        {
            return std::filesystem::path(std::u8string(path.begin(), path.end()));
        }

        std::string GetPresetPath()
        {
            char path[1024] = "";
            std::size_t size = sizeof(path);
            runtime->get_current_preset_path(path, &size);
            return path;
        }

        std::filesystem::file_time_type GetWriteTime(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto time = std::filesystem::last_write_time(path, error);
            return error ? std::filesystem::file_time_type{} : time;
        }

        // the effects, their values and annotations, again
        void Read()
        {
            auto read = std::make_shared<Effects>();
            {
                std::lock_guard lock(mutex);
                read->generation = effects ? effects->generation + 1 : 1;
            }

            // What keys held, by id, through a read (ReShade reloads an effect turned on for the first time, which
            // keeps its values and states): kept, with the preset's own values, but for another preset's
            std::unordered_map<std::string, UniformState> heldUniforms;
            std::unordered_map<std::string, TechniqueState> heldTechniques;
            if (!std::exchange(presetChanged, false))
            {
                std::shared_ptr<const Effects> before;
                {
                    std::lock_guard lock(mutex);
                    before = effects;
                }
                if (before)
                {
                    for (std::size_t i = 0; i < uniforms.size() && i < before->uniforms.size(); ++i)
                        if (uniforms[i].wasHeld)
                            heldUniforms[before->uniforms[i].id] = uniforms[i];
                    for (std::size_t i = 0; i < techniques.size() && i < before->techniques.size(); ++i)
                        if (techniques[i].wasHeld)
                            heldTechniques[before->techniques[i].id] = techniques[i];
                }
            }

            uniforms.clear();
            techniques.clear();
            uniformByHandle.clear();
            techniqueByHandle.clear();
            clockUniforms.clear();

            runtime->enumerate_techniques(nullptr, [&](effect_runtime*, effect_technique handle) {
                char name[256] = "";
                runtime->get_technique_name(handle, name);
                char file[256] = "";
                runtime->get_technique_effect_name(handle, file);
                bool hidden = false;
                if (runtime->get_annotation_bool_from_technique(handle, "hidden", &hidden, 1) && hidden)
                    return;
                std::int32_t hiddenWhole = 0;
                if (runtime->get_annotation_int_from_technique(handle, "hidden", &hiddenWhole, 1) && hiddenWhole)
                    return;

                Technique technique;
                technique.file = file;
                technique.name = name;
                technique.id = technique.file + "/technique:" + technique.name;
                technique.label = TechniqueAnnotation(handle, "ui_label");
                if (technique.label.empty())
                    technique.label = technique.name;
                technique.tooltip = TechniqueAnnotation(handle, "ui_tooltip");

                TechniqueState state{.handle = handle};
                if (const auto held = heldTechniques.find(technique.id); held != heldTechniques.end())
                {
                    state.wasHeld = held->second.wasHeld;
                    state.preset = held->second.preset;
                }
                read->techniqueIndex[technique.id] = read->techniques.size();
                techniqueByHandle[handle.handle] = read->techniques.size();
                read->techniques.push_back(std::move(technique));
                techniques.push_back(state);
            });

            runtime->enumerate_uniform_variables(nullptr, [&](effect_runtime*, effect_uniform_variable handle) {
                char variable[256] = "";
                runtime->get_uniform_variable_name(handle, variable);
                char file[256] = "";
                runtime->get_uniform_variable_effect_name(handle, file);
                format type = format::unknown;
                std::uint32_t rows = 0, columns = 0, arrayLength = 0;
                runtime->get_uniform_variable_type(handle, &type, &rows, &columns, &arrayLength);

                // the clock's, which ReShade sets each frame (E1)
                const auto source = UniformAnnotation(handle, "source");
                if (!source.empty())
                {
                    const UniformState state{.handle = handle, .type = type};
                    if (source == "timer")
                        clockUniforms.emplace_back(state, ClockSource::Timer);
                    else if (source == "frametime")
                        clockUniforms.emplace_back(state, ClockSource::FrameTime);
                    else if (source == "framecount")
                        clockUniforms.emplace_back(state, ClockSource::FrameCount);
                    return;
                }
                // (what ReShade's menu doesn't show either: the hidden, the read-only, matrices and arrays)
                if (UniformFlag(handle, "hidden") || UniformFlag(handle, "noedit") || columns > 1 ||
                    arrayLength > 1 || rows < 1 || rows > 4)
                    return;

                Uniform uniform;
                uniform.file = file;
                uniform.variable = variable;
                uniform.id = uniform.file + "/" + uniform.variable;
                uniform.label = UniformAnnotation(handle, "ui_label");
                if (uniform.label.empty())
                    uniform.label = uniform.variable;
                uniform.category = UniformAnnotation(handle, "ui_category");
                uniform.tooltip = UniformAnnotation(handle, "ui_tooltip");
                uniform.units = UniformAnnotation(handle, "ui_units");
                uniform.components = rows;
                uniform.integer = type == format::r32_sint || type == format::r32_uint;
                const auto uiType = UniformAnnotation(handle, "ui_type");
                const auto min = UniformNumber(handle, "ui_min");
                const auto max = UniformNumber(handle, "ui_max");
                uniform.step = UniformNumber(handle, "ui_step").value_or(0.0f);

                if (type == format::r32_typeless)
                {
                    if (rows != 1)
                        return;
                    uniform.kind = Kind::Bool;
                    uniform.min = 0.0f;
                    uniform.max = 1.0f;
                }
                else if (uniform.integer && rows == 1 && (uiType == "combo" || uiType == "list" || uiType == "radio"))
                {
                    uniform.items = UniformItems(handle);
                    // (a radio without items is a line of text in ReShade's menu)
                    if (uniform.items.empty())
                        return;
                    uniform.kind = Kind::List;
                    uniform.min = 0.0f;
                    uniform.max = static_cast<float>(uniform.items.size() - 1);
                }
                else if (type == format::r32_float && uiType == "color" && rows >= 3)
                {
                    uniform.kind = Kind::Color;
                    uniform.min = 0.0f;
                    uniform.max = 1.0f;
                }
                else if (rows == 1)
                {
                    uniform.kind = min && max && *max > *min ? Kind::Slider : Kind::Drag;
                    uniform.min = uniform.kind == Kind::Slider ? *min : 0.0f;
                    uniform.max = uniform.kind == Kind::Slider ? *max : 0.0f;
                }
                else if (type == format::r32_float && rows <= 3)
                {
                    uniform.kind = Kind::Vector;
                    uniform.min = min.value_or(0.0f);
                    uniform.max = max.value_or(0.0f);
                }
                else
                {
                    return;
                }

                UniformState state{.handle = handle, .type = type};
                if (const auto held = heldUniforms.find(uniform.id); held != heldUniforms.end())
                {
                    state.wasHeld = held->second.wasHeld;
                    state.preset = held->second.preset;
                }
                read->uniformIndex[uniform.id] = read->uniforms.size();
                uniformByHandle[handle.handle] = read->uniforms.size();
                read->uniforms.push_back(std::move(uniform));
                uniforms.push_back(state);
            });

            read->presetPath = GetPresetPath();
            presetWritten = GetWriteTime(ToPath(read->presetPath));

            bool performanceMode = false;
            reshade::get_config_value(nullptr, "GENERAL", "PerformanceMode", performanceMode);
            autoSave = false;
            reshade::get_config_value(nullptr, "OVERLAY", "AutoSavePreset", autoSave);
            status = performanceMode ? Status::PerformanceMode : Status::Ready;

            LOG_INFO("ReShade: {} techniques and {} values, {} clock values, preset {}{}", read->techniques.size(),
                     read->uniforms.size(), clockUniforms.size(), read->presetPath,
                     performanceMode ? ", Performance Mode on" : "");
            std::lock_guard lock(mutex);
            effects = std::move(read);
        }

        // The preset's own values in ReShade for a save of the preset, then the held ones again (Q1). ReShade writes
        // the values of the effects that are on only, leaving the others' as the file has them, so it saves twice:
        // with the effects on as keys have them, so those ReShade saved get the preset's values again, then with the
        // preset's own on and off.
        void SavePresetValues()
        {
            for (std::size_t i = 0; i < uniforms.size(); ++i)
                if (uniforms[i].wasHeld)
                    SetUniform(uniforms[i], effects->uniforms[i].components, uniforms[i].preset);
            runtime->save_current_preset();
            for (auto& technique : techniques)
                if (technique.wasHeld)
                    SetTechnique(technique, technique.preset);
            runtime->save_current_preset();
            for (std::size_t i = 0; i < uniforms.size(); ++i)
                if (uniforms[i].wasHeld)
                    SetUniform(uniforms[i], effects->uniforms[i].components, *uniforms[i].wasHeld);
            for (auto& technique : techniques)
                if (technique.wasHeld)
                    SetTechnique(technique, *technique.wasHeld);
            savedAt = std::chrono::steady_clock::now();
            ++presetRestores;
            LOG_DEBUG("ReShade: gave the preset its own values back for ReShade's save");
        }

        bool AnyHeld()
        {
            return std::ranges::any_of(uniforms, [](const UniformState& each) { return each.wasHeld.has_value(); }) ||
                   std::ranges::any_of(techniques, [](const TechniqueState& each) { return each.wasHeld.has_value(); });
        }

        // The game made its device again (a mod loaded with loadmod): ReShade destroys
        // the old device's runtime and makes another, whose effects are read once it draws them. The old one kept,
        // its next frame called into the runtime destroyed and the game hung.
        void OnDestroyEffectRuntime(effect_runtime* destroyed)
        {
            if (destroyed != runtime)
                return;
            runtime = nullptr;
            readAgain = true;
            LOG_DEBUG("ReShade: its runtime went with the game's device");
        }

        void OnBeginEffects(effect_runtime* drawing, reshade::api::command_list*, reshade::api::resource_view,
                            reshade::api::resource_view)
        {
            // (only once its effects are loaded: see GetReload)
            ++effectFrames;
            drewEffects = true;

            // (one runtime: the game's swap chain)
            if (runtime && drawing != runtime)
                return;
            runtime = drawing;
            if (readAgain)
            {
                Read();
                readAgain = false;
            }

            // the clock (E1), which ReShade sets just before this
            if (!clock || status != Status::Ready)
                return;
            for (const auto& [uniform, source] : clockUniforms)
            {
                settingOurs = true;
                if (source == ClockSource::FrameCount)
                {
                    if (uniform.type == format::r32_float)
                    {
                        const float count = static_cast<float>(clock->frameCount);
                        runtime->set_uniform_value_float(uniform.handle, &count, 1);
                    }
                    else
                    {
                        runtime->set_uniform_value_uint(uniform.handle, &clock->frameCount, 1);
                    }
                }
                else
                {
                    const float value = source == ClockSource::Timer ? clock->timer : clock->frameTime;
                    runtime->set_uniform_value_float(uniform.handle, &value, 1);
                }
                settingOurs = false;
            }
        }

        // The frame's values, at its EndScene, before ReShade draws its effects in Present: setting a technique's
        // state in reshade_begin_effects, in the middle of ReShade drawing them, left ReShade waiting forever for one
        // turned on (its permutation is made after it's on)
        void ApplyFrame()
        {
            if (!runtime)
                return;
            ++frame;
            if (const auto asked = effectsAsked.exchange(-1); asked >= 0)
                runtime->set_effects_state(asked != 0);
            effectsOn = runtime->get_effects_state();
            if (readAgain)
            {
                Read();
                readAgain = false;
            }
            std::shared_ptr<const Effects> now;
            FrameListener frameListener;
            std::vector<std::pair<std::string, Value>> askedValues;
            std::vector<std::pair<std::string, bool>> askedTechniques;
            std::vector<AsMenu> asMenu;
            {
                std::lock_guard lock(mutex);
                now = effects;
                frameListener = listener;
                askedValues = std::exchange(presetValuesAsked, {});
                askedTechniques = std::exchange(presetTechniquesAsked, {});
                asMenu = std::exchange(asMenuAsked, {});
            }
            if (!now || status != Status::Ready)
                return;

            // the preset's own values asked for: of those keys hold, the value they get back
            for (const auto& [id, value] : askedValues)
            {
                if (const auto index = now->FindUniform(id))
                {
                    auto& uniform = uniforms[*index];
                    if (uniform.wasHeld)
                        uniform.preset = value;
                    else
                        SetUniform(uniform, now->uniforms[*index].components, value);
                }
            }
            for (const auto& [id, on] : askedTechniques)
            {
                if (const auto index = now->FindTechnique(id))
                {
                    auto& technique = techniques[*index];
                    if (technique.wasHeld)
                        technique.preset = on;
                    else
                        SetTechnique(technique, on);
                }
            }

            // what keys hold this frame
            for (auto& uniform : uniforms)
                uniform.held.reset();
            for (auto& technique : techniques)
                technique.held.reset();
            clock.reset();
            if (frameListener)
                frameListener(*now);

            for (std::size_t i = 0; i < uniforms.size(); ++i)
            {
                auto& uniform = uniforms[i];
                const auto components = now->uniforms[i].components;
                if (uniform.held)
                {
                    if (!uniform.wasHeld)
                        uniform.preset = GetUniform(uniform, components);
                    if (!uniform.wasHeld || *uniform.wasHeld != *uniform.held ||
                        GetUniform(uniform, components) != *uniform.held)
                        SetUniform(uniform, components, *uniform.held);
                }
                else if (uniform.wasHeld)
                {
                    SetUniform(uniform, components, uniform.preset);
                }
                uniform.wasHeld = uniform.held;
            }
            for (auto& technique : techniques)
            {
                if (technique.held)
                {
                    if (!technique.wasHeld)
                        technique.preset = runtime->get_technique_state(technique.handle);
                    if (runtime->get_technique_state(technique.handle) != *technique.held)
                        SetTechnique(technique, *technique.held);
                }
                else if (technique.wasHeld)
                {
                    SetTechnique(technique, technique.preset);
                }
                technique.wasHeld = technique.held;
            }

            // ReShade saved the preset while keys held values (after a change in its menu, or written in the last
            // frames by a save of its own the events don't tell, as "Reset all to default"): the preset's own
            // values go in instead, before the file is written at the end of this frame
            if (frame % 30 == 0)
            {
                const auto written = GetWriteTime(ToPath(now->presetPath));
                if (written != presetWritten)
                {
                    presetWritten = written;
                    if (std::chrono::steady_clock::now() - savedAt > std::chrono::seconds(3))
                        restorePreset = true;
                }
            }
            if (saveAsked.exchange(false) && autoSave)
            {
                if (AnyHeld())
                {
                    SavePresetValues();
                }
                else
                {
                    runtime->save_current_preset();
                    savedAt = std::chrono::steady_clock::now();
                }
                restorePreset = false;
            }
            if (restorePreset)
            {
                if (AnyHeld())
                    SavePresetValues();
                restorePreset = false;
            }

            // (the harness's) as ReShade's menu sets them, which this frame sees
            for (const auto& each : asMenu)
            {
                if (const auto index = now->FindUniform(each.id))
                {
                    // (not as the mod's own: the events take it for the menu's)
                    SetUniform(uniforms[*index], now->uniforms[*index].components, each.value, false);
                }
                else if (const auto technique = now->FindTechnique(each.id))
                {
                    runtime->set_technique_state(techniques[*technique].handle, each.value[0] >= 0.5f);
                }
                if (each.save)
                    runtime->save_current_preset();
            }

            // the values for the other threads
            Values read;
            read.generation = now->generation;
            read.preset.reserve(uniforms.size());
            read.drawn.reserve(uniforms.size());
            for (std::size_t i = 0; i < uniforms.size(); ++i)
            {
                const auto drawn = GetUniform(uniforms[i], now->uniforms[i].components);
                read.drawn.push_back(drawn);
                read.preset.push_back(uniforms[i].wasHeld ? uniforms[i].preset : drawn);
            }
            for (const auto& technique : techniques)
            {
                const bool on = runtime->get_technique_state(technique.handle);
                read.drawnOn.push_back(on);
                read.presetOn.push_back(technique.wasHeld ? technique.preset : on);
            }
            std::lock_guard lock(mutex);
            values = std::move(read);
        }

        // ReShade fires this as a load of its effects starts, the old ones gone, and again once they're all made: the
        // first with techniques (no effect file at all has none either time, so the second counts too). A reset of the
        // device between the two waits inside ReShade for the compiles.
        void OnReloadedEffects(effect_runtime* reloaded)
        {
            bool anyTechnique = false;
            reloaded->enumerate_techniques(nullptr, [&](effect_runtime*, effect_technique) { anyTechnique = true; });
            const bool ended = anyTechnique || ++loadEvents >= 2;
            loading = !ended;
            if (ended)
                loadEvents = 0;

            if (runtime && reloaded != runtime)
                return;
            // (the handles are new)
            readAgain = true;
            if (!ended)
                return;
            // (with its toggle off reshade_begin_effects doesn't fire, and a reset took the runtime)
            runtime = reloaded;
            effectsOn = reloaded->get_effects_state();
        }

        void OnSetCurrentPresetPath(effect_runtime* changed, const char*)
        {
            if (runtime && changed != runtime)
                return;
            readAgain = true;
            presetChanged = true;
        }

        bool OnReorderTechniques(effect_runtime* reordered, std::size_t, effect_technique*)
        {
            if (runtime && reordered != runtime)
                return false;
            readAgain = true;
            // (ReShade saves the preset after it)
            restorePreset = true;
            return false;
        }

        bool OnSetUniformValue(effect_runtime* changed, effect_uniform_variable variable, const void* data,
                               std::size_t size)
        {
            if (settingOurs || changed != runtime || readAgain)
                return false;
            const auto found = uniformByHandle.find(variable.handle);
            if (found == uniformByHandle.end())
                return false;
            const auto& uniform = uniforms[found->second];
            std::shared_ptr<const Effects> now;
            {
                std::lock_guard lock(mutex);
                now = effects;
            }
            if (!now || found->second >= now->uniforms.size())
                return false;

            MenuChange change{.id = now->uniforms[found->second].id};
            const auto count = std::min<std::size_t>(size / 4, 4);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto bytes = static_cast<const std::uint8_t*>(data) + i * 4;
                switch (uniform.type)
                {
                    case format::r32_float:
                    {
                        float value;
                        std::memcpy(&value, bytes, 4);
                        change.value[i] = value;
                        break;
                    }
                    case format::r32_sint:
                    {
                        std::int32_t value;
                        std::memcpy(&value, bytes, 4);
                        change.value[i] = static_cast<float>(value);
                        break;
                    }
                    default:
                    {
                        std::uint32_t value;
                        std::memcpy(&value, bytes, 4);
                        change.value[i] = static_cast<float>(value != 0 && uniform.type != format::r32_uint ? 1
                                                                                                            : value);
                        break;
                    }
                }
            }
            // (ReShade saves the preset after a change in its menu, so with keys holding values it gets the preset's
            // own next frame)
            restorePreset = true;
            std::lock_guard lock(mutex);
            menuChanges.push_back(std::move(change));
            return false;
        }

        bool OnSetTechniqueState(effect_runtime* changed, effect_technique handle, bool enabled)
        {
            if (settingOurs || changed != runtime || readAgain)
                return false;
            const auto found = techniqueByHandle.find(handle.handle);
            std::shared_ptr<const Effects> now;
            {
                std::lock_guard lock(mutex);
                now = effects;
            }
            if (found == techniqueByHandle.end() || !now || found->second >= now->techniques.size())
                return false;
            restorePreset = true;
            std::lock_guard lock(mutex);
            menuChanges.push_back(
                {.id = now->techniques[found->second].id, .value = {enabled ? 1.0f : 0.0f}, .technique = true});
            return false;
        }
    }  // namespace

    void Initialize()
    {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Initialize), &module);
        if (!reshade::register_addon(module))
        {
            LOG_WARN("ReShade: it didn't take the mod as an add-on, so its values can't be keyed");
            return;
        }
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
        reshade::register_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(&OnReloadedEffects);
        reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(&OnSetCurrentPresetPath);
        reshade::register_event<reshade::addon_event::reshade_reorder_techniques>(&OnReorderTechniques);
        reshade::register_event<reshade::addon_event::reshade_set_uniform_value>(&OnSetUniformValue);
        reshade::register_event<reshade::addon_event::reshade_set_technique_state>(&OnSetTechniqueState);
        reshade::get_config_value(nullptr, "GENERAL", "NoReloadOnInit", noReloadOnInit);
        status = Status::Waiting;
        LOG_INFO("ReShade: registered as an add-on");
    }

    void Shutdown()
    {
        if (status == Status::NotFound)
            return;
        if (runtime)
        {
            std::shared_ptr<const Effects> now;
            {
                std::lock_guard lock(mutex);
                now = effects;
            }
            for (std::size_t i = 0; now && i < uniforms.size(); ++i)
                if (uniforms[i].wasHeld)
                    SetUniform(uniforms[i], now->uniforms[i].components, uniforms[i].preset);
            for (auto& technique : techniques)
                if (technique.wasHeld)
                    SetTechnique(technique, technique.preset);
        }
        uniforms.clear();
        techniques.clear();
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
        reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(&OnBeginEffects);
        reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(&OnReloadedEffects);
        reshade::unregister_event<reshade::addon_event::reshade_set_current_preset_path>(&OnSetCurrentPresetPath);
        reshade::unregister_event<reshade::addon_event::reshade_reorder_techniques>(&OnReorderTechniques);
        reshade::unregister_event<reshade::addon_event::reshade_set_uniform_value>(&OnSetUniformValue);
        reshade::unregister_event<reshade::addon_event::reshade_set_technique_state>(&OnSetTechniqueState);
        reshade::unregister_addon(module);
        runtime = nullptr;
        status = Status::NotFound;
        LOG_INFO("ReShade: unregistered");
    }

    void BeforePresent()
    {
        ApplyFrame();
    }

    void OnDeviceReset()
    {
        effectFramesAtReset = effectFrames.load();
        loadEvents = 0;
        loading = status != Status::NotFound && !noReloadOnInit;
    }

    void OnDeviceCreated()
    {
        // (the old device's runtime can outlive it, and the mod would go on shutting out the new one's events)
        runtime = nullptr;
        readAgain = true;
        OnDeviceReset();
    }

    bool IsDrawingEffects()
    {
        return status != Status::NotFound && effectFrames > 0 && effectsOn;
    }

    Reload GetReload()
    {
        return {.present = status != Status::NotFound,
                .effectsOn = effectsOn,
                .loading = loading || GetTickCount64() < loadingUntil,
                .framesSince = effectFrames - effectFramesAtReset};
    }

    void SetEffectsOn(bool on)
    {
        effectsAsked = on ? 1 : 0;
    }

    void SetLoadingFor(std::uint32_t ms)
    {
        loadingUntil = GetTickCount64() + ms;
    }

    bool TakeDrewEffects()
    {
        return drewEffects.exchange(false);
    }

    Status GetStatus()
    {
        return status;
    }

    std::shared_ptr<const Effects> GetEffects()
    {
        std::lock_guard lock(mutex);
        return effects;
    }

    Values GetValues()
    {
        std::lock_guard lock(mutex);
        return values;
    }

    void SetFrameListener(FrameListener frameListener)
    {
        std::lock_guard lock(mutex);
        listener = std::move(frameListener);
    }

    void Hold(std::size_t uniform, const Value& value)
    {
        if (uniform < uniforms.size())
            uniforms[uniform].held = value;
    }

    void HoldTechnique(std::size_t technique, bool on)
    {
        if (technique < techniques.size())
            techniques[technique].held = on;
    }

    void SetClock(const Clock& frameClock)
    {
        clock = frameClock;
    }

    void SetPresetValue(const std::string& id, const Value& value)
    {
        std::lock_guard lock(mutex);
        presetValuesAsked.emplace_back(id, value);
    }

    void SetPresetTechnique(const std::string& id, bool on)
    {
        std::lock_guard lock(mutex);
        presetTechniquesAsked.emplace_back(id, on);
    }

    void SavePreset()
    {
        saveAsked = true;
    }

    void SetAsMenu(const std::string& id, const Value& value, bool save)
    {
        std::lock_guard lock(mutex);
        asMenuAsked.push_back({id, value, save});
    }

    std::vector<MenuChange> TakeMenuChanges()
    {
        std::lock_guard lock(mutex);
        return std::exchange(menuChanges, {});
    }

    std::uint32_t GetPresetRestores()
    {
        return presetRestores;
    }
}  // namespace IWXMVM::ReShade
