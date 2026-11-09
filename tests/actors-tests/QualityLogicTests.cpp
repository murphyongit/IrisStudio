#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/QualityLogic.hpp"

using namespace IWXMVM::Components;
using QualityLogic::ShadowQuality;

// The Look tab's Quality section and the note when the game's effects run out

TEST("quality: the shadow map sizes and their names")
{
    CHECK(QualityLogic::GetShadowMapSize(ShadowQuality::Game) == 1024);
    CHECK(QualityLogic::GetShadowMapSize(ShadowQuality::High) == 2048);
    CHECK(QualityLogic::GetShadowMapSize(ShadowQuality::Max) == 4096);
    for (const auto quality : QualityLogic::SHADOW_QUALITIES)
        CHECK(QualityLogic::FindShadowQuality(QualityLogic::GetName(quality)) == quality);
    CHECK(QualityLogic::FindShadowQuality("max") == ShadowQuality::Max);
    CHECK(!QualityLogic::FindShadowQuality("ultra"));
}

TEST("quality: a shadow distance keeps its reach whatever the map's size, the value only shrinking (sharper)")
{
    // the game's 0.25 at 1,024 reaches about 1,100, as measured (7.1); 0.25 at 4,096 about 4,500 (7.9)
    CHECK_NEAR(QualityLogic::GetShadowDistance(0.25f, 1024), 1126.4, 0.1);
    CHECK_NEAR(QualityLogic::GetShadowDistance(0.25f, 4096), 4505.6, 0.1);
    // 3,300 units: 0.73 with the game's map, 0.18 with Max's (sharp at 800 and 3,000 in the frames)
    CHECK_NEAR(QualityLogic::GetSampleSize(3300.0f, 1024), 0.7324, 0.001);
    CHECK_NEAR(QualityLogic::GetSampleSize(3300.0f, 4096), 0.1831, 0.001);
    for (const auto size : {1024, 2048, 4096})
        CHECK_NEAR(QualityLogic::GetShadowDistance(QualityLogic::GetSampleSize(5000.0f, size), size), 5000.0, 0.5);
    // never below what the game takes: the shortest at Max is about 1,130
    CHECK(QualityLogic::GetSampleSize(QualityLogic::MIN_SHADOW_DISTANCE, 4096) == QualityLogic::MIN_SAMPLE_SIZE);
    CHECK(QualityLogic::GetSampleSize(0.0f, 1024) == QualityLogic::MIN_SAMPLE_SIZE);
}

TEST("quality: this PC's values read and write, what doesn't read the game's, an older config's Capture let go")
{
    const QualityLogic::Settings settings{ShadowQuality::High, true};
    const auto json = QualityLogic::ToJson(settings);
    CHECK(json["scene"]["shadowQuality"] == "High" && json["textures"] == "Pack" && !json.contains("capture"));
    CHECK(QualityLogic::FromJson(json) == settings);
    CHECK(QualityLogic::FromJson(nlohmann::json::object()) == QualityLogic::Settings{});
    CHECK(QualityLogic::FromJson(nlohmann::json::array()) == QualityLogic::Settings{});
    const auto odd = QualityLogic::FromJson({{"scene", {{"shadowQuality", "ultra"}}}, {"textures", true}});
    CHECK(odd.sceneShadows == ShadowQuality::Game && !odd.texturePack);
    // the Capture column (gone 2026-11-02) of an older config: the Scene value stays, Capture's is let go
    const auto older = QualityLogic::FromJson(
        {{"scene", {{"shadowQuality", "High"}}}, {"capture", {{"shadowQuality", "Max"}}}});
    CHECK(older.sceneShadows == ShadowQuality::High && !older.texturePack);
}

TEST("effects note: once when spawns are refused, again only after 10 s while it goes on, anew with each map")
{
    QualityLogic::EffectsNote note;
    CHECK(!QualityLogic::ShouldNoteRefusedEffects(note, 0, 1000));
    CHECK(QualityLogic::ShouldNoteRefusedEffects(note, 3, 2000));
    // more refused soon after: not again; nothing new: not again
    CHECK(!QualityLogic::ShouldNoteRefusedEffects(note, 9, 5000));
    CHECK(!QualityLogic::ShouldNoteRefusedEffects(note, 9, 30000));
    // still running out 10 s on
    CHECK(QualityLogic::ShouldNoteRefusedEffects(note, 12, 31000));
    // another map: the count started again
    CHECK(!QualityLogic::ShouldNoteRefusedEffects(note, 0, 32000));
    CHECK(!QualityLogic::ShouldNoteRefusedEffects(note, 1, 33000));
    CHECK(QualityLogic::ShouldNoteRefusedEffects(note, 2, 42000));
}
