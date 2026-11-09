#pragma once

#include "nlohmann/json.hpp"

namespace IWXMVM::Components::QualityLogic
{
    // The Look tab's Quality section, apart from the game and the UI so the tests in
    // tests/actors-tests build it on their own. Shadow distance is how far the sun's shadows reach, a part of the shot's
    // look (keyframeable, with the Look presets); Shadow quality is the size of the sun's shadow map, this PC's (the
    // editor's settings), with a value while working (Scene) and one while Record runs (Capture). A bigger map keeps the
    // distance and sharpens the shadows.

    enum class ShadowQuality
    {
        Game,  // the game's 1,024
        High,  // 2,048
        Max,   // 4,096
    };
    inline constexpr std::array<ShadowQuality, 3> SHADOW_QUALITIES{ShadowQuality::Game, ShadowQuality::High,
                                                                   ShadowQuality::Max};
    // the size of each half of the sun's shadow map: 1,024, 2,048, 4,096
    std::int32_t GetShadowMapSize(ShadowQuality quality);
    // "Game", "High", "Max", as the section shows them; read back ignoring case
    std::string_view GetName(ShadowQuality quality);
    std::optional<ShadowQuality> FindShadowQuality(std::string_view name);

    // How far shadows reach with the game's sm_sunSampleSizeNear at a map size, measured in game (7.1, 7.9): 4.4 units
    // per texel of the map per unit of the value. The game refuses a value under MIN_SAMPLE_SIZE.
    inline constexpr float REACH_PER_SAMPLE = 4.4f;
    inline constexpr float MIN_SAMPLE_SIZE = 0.0625f;
    // the slider's range: the game's own reach (0.25 at 1,024) up
    inline constexpr float MIN_SHADOW_DISTANCE = 1100.0f;
    inline constexpr float MAX_SHADOW_DISTANCE = 10000.0f;
    // the value of sm_sunSampleSizeNear that makes shadows reach a distance with a map size, at least MIN_SAMPLE_SIZE
    float GetSampleSize(float distance, std::int32_t mapSize);
    // and the distance a value reaches with a map size
    float GetShadowDistance(float sampleSize, std::int32_t mapSize);

    // This PC's values: the Shadow quality and whether a texture pack draws (Textures: Pack),
    // the same while recording (the Capture column went 2026-11-02)
    struct Settings
    {
        ShadowQuality sceneShadows = ShadowQuality::Game;
        bool texturePack = false;

        bool operator==(const Settings&) const = default;
    };
    // {"scene": {"shadowQuality": "Game"}, "textures": "Pack"}; what isn't there or doesn't read is the game's (an
    // older config's "capture" is let go)
    nlohmann::json ToJson(const Settings& settings);
    Settings FromJson(const nlohmann::json& json);

    // The note when the game's effects run out: shown when the game refused spawns
    // since it last looked (with every effect in use), at most once every NOTE_INTERVAL while it goes on. The count
    // starts again with each map: one lower than seen is a new map's.
    inline constexpr std::uint64_t EFFECTS_NOTE_INTERVAL_MS = 10000;
    struct EffectsNote
    {
        std::uint32_t seen = 0;
        std::optional<std::uint64_t> shownAt;
    };
    // whether to show the note now, refused being the game's count and now in milliseconds
    bool ShouldNoteRefusedEffects(EffectsNote& note, std::uint32_t refused, std::uint64_t now);
}  // namespace IWXMVM::Components::QualityLogic
