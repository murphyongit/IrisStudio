#include "StdInclude.hpp"
#include "QualityLogic.hpp"

namespace IWXMVM::Components::QualityLogic
{
    std::int32_t GetShadowMapSize(ShadowQuality quality)
    {
        switch (quality)
        {
            case ShadowQuality::High:
                return 2048;
            case ShadowQuality::Max:
                return 4096;
            default:
                return 1024;
        }
    }

    std::string_view GetName(ShadowQuality quality)
    {
        switch (quality)
        {
            case ShadowQuality::High:
                return "High";
            case ShadowQuality::Max:
                return "Max";
            default:
                return "Game";
        }
    }

    std::optional<ShadowQuality> FindShadowQuality(std::string_view name)
    {
        for (const auto quality : SHADOW_QUALITIES)
        {
            const auto known = GetName(quality);
            if (std::ranges::equal(known, name, [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                }))
                return quality;
        }
        return std::nullopt;
    }

    float GetSampleSize(float distance, std::int32_t mapSize)
    {
        return std::max(distance / (REACH_PER_SAMPLE * static_cast<float>(std::max(mapSize, 1))), MIN_SAMPLE_SIZE);
    }

    float GetShadowDistance(float sampleSize, std::int32_t mapSize)
    {
        return sampleSize * REACH_PER_SAMPLE * static_cast<float>(mapSize);
    }

    namespace
    {
        constexpr std::string_view SHADOW_QUALITY = "shadowQuality";

        ShadowQuality ReadShadows(const nlohmann::json& json, std::string_view part)
        {
            const auto values = json.find(part);
            if (values == json.end() || !values->is_object())
                return ShadowQuality::Game;
            const auto value = values->find(SHADOW_QUALITY);
            if (value == values->end() || !value->is_string())
                return ShadowQuality::Game;
            return FindShadowQuality(value->get<std::string>()).value_or(ShadowQuality::Game);
        }
    }  // namespace

    nlohmann::json ToJson(const Settings& settings)
    {
        return {{"scene", {{SHADOW_QUALITY, GetName(settings.sceneShadows)}}},
                {"textures", settings.texturePack ? "Pack" : "Game"}};
    }

    Settings FromJson(const nlohmann::json& json)
    {
        if (!json.is_object())
            return {};
        const auto textures = json.find("textures");
        const bool pack = textures != json.end() && textures->is_string() && textures->get<std::string>() == "Pack";
        return {ReadShadows(json, "scene"), pack};
    }

    bool ShouldNoteRefusedEffects(EffectsNote& note, std::uint32_t refused, std::uint64_t now)
    {
        if (refused <= note.seen)
        {
            note.seen = refused;
            return false;
        }
        note.seen = refused;
        if (note.shownAt && now - *note.shownAt < EFFECTS_NOTE_INTERVAL_MS)
            return false;
        note.shownAt = now;
        return true;
    }
}  // namespace IWXMVM::Components::QualityLogic
