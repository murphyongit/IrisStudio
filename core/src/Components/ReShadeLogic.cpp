#include "StdInclude.hpp"
#include "ReShadeLogic.hpp"

namespace IWXMVM::Components::ReShadeLogic
{
    std::string GetEffectName(const ReShade::Effects& effects, const std::string& file)
    {
        for (const auto& technique : effects.techniques)
        {
            if (technique.file == file)
                return technique.label;
        }
        return file.ends_with(".fx") ? file.substr(0, file.size() - 3) : file;
    }

    RuntimeProperties::Description Describe(const ReShade::Uniform& uniform, const std::string& effectName)
    {
        RuntimeProperties::Description description;
        description.id = uniform.id;
        description.name = std::format("ReShade: {} > {}", effectName, uniform.label);
        description.min = uniform.min;
        description.max = uniform.max > uniform.min ? uniform.max : uniform.min + 1.0f;
        switch (uniform.kind)
        {
            case ReShade::Kind::Bool:
                description.steps = RuntimeProperties::Steps::Switch;
                break;
            case ReShade::Kind::List:
                description.steps = RuntimeProperties::Steps::Whole;
                break;
            case ReShade::Kind::Color:
                description.valueType = Types::KeyframeValueType::Vector3;
                description.valueNames = {"R", "G", "B"};
                break;
            case ReShade::Kind::Vector:
                description.valueType = Types::KeyframeValueType::Vector3;
                description.valueNames = {"X", "Y", uniform.components > 2 ? "Z" : ""};
                break;
            default:
                break;
        }
        return description;
    }

    RuntimeProperties::Description Describe(const ReShade::Technique& technique)
    {
        RuntimeProperties::Description description;
        description.id = technique.id;
        description.name = std::format("ReShade: {} on", technique.label);
        description.steps = RuntimeProperties::Steps::Switch;
        return description;
    }

    Types::KeyframeValue ToKey(const ReShade::Uniform& uniform, const ReShade::Value& value)
    {
        if (uniform.kind == ReShade::Kind::Color || uniform.kind == ReShade::Kind::Vector)
            return Types::KeyframeValue(glm::vec3(value[0], value[1], value[2]));
        return Types::KeyframeValue(value[0]);
    }

    ReShade::Value FromKey(const ReShade::Uniform& uniform, const Types::KeyframeValue& key,
                           const ReShade::Value& preset)
    {
        // (a colour's alpha, and what else a key doesn't hold, as the preset has it)
        auto value = preset;
        if (uniform.kind == ReShade::Kind::Color || uniform.kind == ReShade::Kind::Vector)
        {
            for (std::uint32_t i = 0; i < std::min(uniform.components, 3u); ++i)
                value[i] = key.vector3[static_cast<int>(i)];
            return value;
        }
        auto scalar = key.floatingPoint;
        switch (uniform.kind)
        {
            case ReShade::Kind::Bool:
                scalar = scalar >= 0.5f ? 1.0f : 0.0f;
                break;
            case ReShade::Kind::List:
                scalar = std::clamp(std::round(scalar), 0.0f, uniform.max);
                break;
            default:
                if (uniform.integer)
                    scalar = std::round(scalar);
                break;
        }
        value[0] = scalar;
        return value;
    }

    int OutOfRange(const ReShade::Uniform& uniform, float value)
    {
        if (uniform.kind != ReShade::Kind::Slider || uniform.max <= uniform.min)
            return 0;
        // (as the slider shows it)
        const auto margin = (uniform.max - uniform.min) * 1e-4f;
        if (value > uniform.max + margin)
            return 1;
        if (value < uniform.min - margin)
            return -1;
        return 0;
    }

    std::pair<float, float> GetSliderRange(const ReShade::Uniform& uniform, float value, float preset)
    {
        if (uniform.max > uniform.min)
            return {uniform.min, uniform.max};
        // (a Drag: from 0 to twice the larger of the two, or around them when below 0)
        const auto low = std::min({0.0f, value, preset});
        const auto high = std::max({value, preset, 0.0f});
        const auto span = std::max(high - low, 1.0f);
        return low < 0.0f ? std::pair(low - span, high + span) : std::pair(0.0f, std::max(high * 2.0f, 1.0f));
    }

    Folds GetFolds(const ReShade::Effects& effects, const std::vector<bool>& on, const std::set<std::string>& keyedIds,
                   std::set<std::string>& kept)
    {
        std::set<std::string> keyedFiles;
        for (const auto& id : keyedIds)
            keyedFiles.insert(GetFile(id));

        Folds folds;
        for (std::size_t i = 0; i < effects.techniques.size(); ++i)
        {
            const auto& technique = effects.techniques[i];
            const bool listed = (i < on.size() && on[i]) || keyedIds.contains(technique.id) ||
                                keyedFiles.contains(technique.file);
            if (listed)
                kept.insert(technique.id);
            else if (kept.contains(technique.id))
                ++folds.kept;
            (kept.contains(technique.id) ? folds.listed : folds.others).push_back({i});
        }

        // each file's values under its first technique listed
        std::set<std::string> placed;
        for (auto* list : {&folds.listed, &folds.others})
        {
            for (auto& fold : *list)
            {
                const auto& file = effects.techniques[fold.technique].file;
                if (!placed.insert(file).second)
                    continue;
                for (std::size_t u = 0; u < effects.uniforms.size(); ++u)
                {
                    if (effects.uniforms[u].file == file)
                        fold.uniforms.push_back(u);
                }
            }
        }
        return folds;
    }

    std::string GetFile(const std::string& id)
    {
        const auto slash = id.find('/');
        return slash == std::string::npos ? id : id.substr(0, slash);
    }

    std::vector<Missing> GetMissing(const ReShade::Effects& effects, const std::vector<std::string>& keyedIds)
    {
        std::vector<Missing> missing;
        for (const auto& id : keyedIds)
        {
            if (effects.FindUniform(id) || effects.FindTechnique(id))
                continue;
            const auto file = GetFile(id);
            auto found = std::ranges::find(missing, file, &Missing::file);
            if (found == missing.end())
            {
                missing.push_back({file});
                found = missing.end() - 1;
            }
            found->ids.push_back(id);
        }
        return missing;
    }

    ReShade::Clock AdvanceClock(ClockState& state, std::uint32_t tick)
    {
        ReShade::Clock clock;
        clock.timer = static_cast<float>(tick);
        if (state.tick && *state.tick != tick)
        {
            ++state.frameCount;
            // (back after a rewind: as a frame of its own)
            clock.frameTime = tick > *state.tick ? static_cast<float>(tick - *state.tick) : 0.0f;
        }
        state.tick = tick;
        clock.frameCount = state.frameCount;
        return clock;
    }
}  // namespace IWXMVM::Components::ReShadeLogic
