#pragma once
#include "Types/AnimationCatalog.hpp"

#include <span>
#include <unordered_set>

#include "nlohmann/json.hpp"

namespace IWXMVM::AnimationCatalogLogic
{
    // The catalog of every animation the game has: reading it, which of its animations
    // fit an actor's models and what each is. No game state; tested in tests/actors-tests.

    // Reads the catalog (resources/animations.json, version 1, see tools/t5-characters/animations.py); throws on one it
    // can't read. Each entry's death and alert are worked out here, with ActorLogic's rules.
    Types::AnimationCatalog Parse(const nlohmann::json& root);
    // Whether an animation is one the catalog lists: all but those of the first person arms (viewmodel_*, and those
    // PoseLogic::ClassifyAnimation calls arms without a body), which actors never play
    bool IsListed(std::string_view name, Types::AnimationFamily family);

    // Which animations fit a model: those of the rig its bones make up, nearly all
    // of whose bones of that rig it has. A human's animations (MP's, SP's, the face's of a body) fit a human's body and
    // a dog's a dog: FIT_SHARE of the rig's bones they have tracks for must be among the model's, counting the spine
    // rig's (pelvis and torso_stabilizer) on a body of the SP rig, which the game gets when it plays MP's animations
    // (see ActorLogic and t5/src/Actors.cpp). What they have tracks for beyond the rig (clothes, gear, guns, the camera
    // of cutscenes) doesn't count. A face's animations fit every model with a face, a prop's those with FIT_SHARE of its
    // bones; one with no bones fits nothing.
    struct ModelRig
    {
        std::unordered_set<std::string> bones;  // with the spine rig's on a body of the SP rig
        bool human = false;                     // a human's body (j_spinelower; a head has none), not a dog's
        bool dog = false;                       // a dog's (j_tail..., j_pelvis)
        bool face = false;                      // a human's face (j_jaw, j_lip_top_le or j_brow_le)
        // a killstreak's (told by its model, not its bones: see Components::Actors::GetFits), which the game moves in
        // code: only its own animations fit it (the Huey's doors), none of the props' that key its root bones alone
        bool vehicle = false;
        std::vector<std::string> own;  // those, by the game's names
    };
    // the rig of a model's bones (or of all of an actor's models together)
    ModelRig GetModelRig(std::span<const std::string> bones);
    inline constexpr float FIT_SHARE = 0.95f;
    bool Fits(const Types::AnimationCatalog& catalog, const Types::AnimationEntry& entry, const ModelRig& rig);
    // Why an animation doesn't fit a model, in words for the UI ("Made for a dog's skeleton: the actor is a person");
    // empty when it fits
    std::string DescribeMisfit(const Types::AnimationCatalog& catalog, const Types::AnimationEntry& entry,
                               const ModelRig& rig);
    // whether each of the catalog's entries fits (in their order): quicker than Fits on each, as they share bone sets
    std::vector<bool> GetFits(const Types::AnimationCatalog& catalog, const ModelRig& rig);

    // The entries whose name or zone (or the zone's name) has each word of a search (any case), in the catalog's order
    bool Matches(const Types::AnimationCatalog& catalog, const Types::AnimationEntry& entry, std::string_view search);
    std::vector<const Types::AnimationEntry*> Search(const Types::AnimationCatalog& catalog, std::string_view search);

    // What an animation is, for the browser: a face's alone (Faces), a death, a dog's or an animal's, a prop's, one
    // that moves or changes stance (Movement) as the catalog says; else as its name says (Pain and reactions, Melee,
    // Cover, Movement, Combat, Idle and poses, Other). An animation the catalog doesn't have goes by its kind and name.
    std::string_view GetCategory(std::string_view name, Types::AnimationKind kind, const Types::AnimationEntry* entry);
    // the categories, in the order to list them
    const std::vector<std::string_view>& GetCategories();

    // The animations the browser lists: the killstreaks' motions (see KillstreakLogic) the game's catalog lists, then
    // every entry of the catalog, with the game's copy where the game has it loaded (which plays), then those the game
    // has loaded that the catalog doesn't have; each with what it is and where it is from
    struct Listed
    {
        std::string_view name;
        const Types::AnimationEntry* entry = nullptr;            // the catalog's, if it has it
        const Types::ActorCatalog::Animation* loaded = nullptr;  // the game's, if it has it loaded
        std::size_t index = SIZE_MAX;                            // of the entry in the catalog (see GetFits)
        Types::AnimationKind kind = Types::AnimationKind::Other;
        std::size_t category = 0;  // in GetCategories
        std::size_t heading = 0;   // in AnimationList::headings
        std::string lowerName;     // for searches: the name and where it is from (zones and their names), lower case
        std::string lowerWhere;
    };
    struct AnimationList
    {
        std::vector<Listed> animations;     // by where they are from (as the catalog lists its sources), then name
        std::vector<std::string> headings;  // where they are from: "Multiplayer", "S.O.G. (Campaign)"; the game's
                                            // own the catalog doesn't have are "Loaded in the game"
    };
    // The pointers are into both catalogs, which must outlive it
    AnimationList List(const Types::AnimationCatalog& catalog, const Types::ActorCatalog& game);
    // the heading of a source: its group for a group's common zones ("Campaign"), else "S.O.G. (Campaign)"
    std::string GetHeading(const Types::AnimationSource& source);

    // the stance a posture stands in for the legs (Back lies: Prone), none for Unknown
    std::optional<Types::ActorStance> GetStance(Types::AnimationPosture posture);
    std::string_view GetPostureName(Types::AnimationPosture posture);
    std::string_view GetMovementName(Types::AnimationMovement movement);
    // "stand stop", "crouch -", "-" for nothing known
    std::string DescribeState(const Types::AnimationState& state);
}  // namespace IWXMVM::AnimationCatalogLogic
