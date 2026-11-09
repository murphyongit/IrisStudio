#pragma once
#include "Structures.hpp"
#include "Types/Actor.hpp"
#include "Types/BoneCamera.hpp"
#include "Components/ActorLogic.hpp"

namespace IWXMVM::T5::Actors
{
    // Draws the actors of Components::Actors and tells core what they can be made of (see Actors.cpp)

    // frees what the game holds for the actors, before the models they are made of go away; they are built again
    // from core's actors in the next frame. Main thread only.
    void ReleaseAll();
    // As the mod unloads (T5Interface::PrepareUnload): stops the actors' effects, builds and draws them no more, and
    // leaves their DObjs to the game (which frees them with the map) and the animations made up and models copied for
    // them to the process. Main thread only.
    void Unload();

    // A player's model that isn't an actor's: the spine rig of a body, null for one that
    // doesn't need one; an entry of animations made up or copied for it made an animation; and the animation that
    // lines up the stabilizer of a DObj for an animation made for SP bodies (like), null when it has none. Those
    // made go with the map's models. Main thread only.
    const Structures::XModel* GetSpineRigOf(const Structures::XModel& body);
    // a body's model with its gun tag where a soldier's rests (GripLogic::SOLDIER_GUN_TAG), the model itself if it has
    // it there or has none; the copies go with the map's models
    Structures::XModel* GetHoldingModel(Structures::XModel* model);
    // a campaign or Zombies body as a player's first model: a copy of it with the bone MP animations need, which the
    // game's IK can run on (see Actors.cpp); the model itself for others; the copies go with the map's models
    Structures::XModel* GetPlayerBody(Structures::XModel* model);
    void SetPlayerBodiesOn(bool on);  // for the harness: off, players' campaign bodies go back to the spine rig
    void SetAnimationEntry(Structures::XAnim_s* anims, std::uint32_t index, const Structures::XAnimParts* parts);
    const Structures::XAnimParts* GetLinedSpine(const Structures::DObj& obj, const Structures::XAnimParts& like);

    // stops the effects actors started (muzzle flashes, blood, effect events), e.g. when the demo rewinds
    void StopEffects();

    // the effects actors started that are still playing, for the harness
    std::size_t CountLiveEffects();
    // a killstreak actor's running effects playing now (its rotors, lights...), as "effect@tag"
    std::vector<std::string> GetRunningEffects(std::uint32_t id);
    // how many of each of the killstreaks' dust and tread effects played, since the
    // last reset
    std::map<std::string, std::size_t> GetKillstreakEffectCounts(bool reset);
    // which way the effects with a part in their name last played, for the harness: their axes' first
    std::optional<glm::vec3> GetKillstreakEffectForward(const std::string& part);
    // An effect in the scene as the game plays it now, for the harness: its looping plays
    // kept, how many of those still play, and how many plays were spawned since the playhead last jumped; none for one
    // that plays nothing. Main thread only.
    struct SceneEffectState
    {
        std::size_t kept = 0;
        std::size_t alive = 0;
        std::size_t begun = 0;
    };
    std::optional<SceneEffectState> GetSceneEffectState(std::uint32_t id);
    // how many plays of each effect the scene's effects spawned since the last reset
    std::map<std::string, std::size_t> GetSceneEffectSpawns(bool reset);

    // the shots and sounds actors played from their animations' notetracks since the counts were reset, for the
    // harness. Main thread only.
    struct NotetrackCounts
    {
        std::size_t shots = 0;
        std::size_t sounds = 0;
    };
    NotetrackCounts GetNotetrackCounts(bool reset);
    // the shots actors' guns fired (Fire events and fire notetracks) since the counts were reset, by the bone they
    // came from (tag_flash, a dual wield gun's left one tag_flash1), for the harness. Main thread only.
    std::map<std::string, std::size_t> GetMuzzleCounts(bool reset);
    // what actors' bullets left where they landed since the counts were reset, by what they hit and the effect played
    // (world, player or actor: the surface and the effect's name), for the harness; and where the last one landed.
    // Main thread only.
    std::map<std::string, std::size_t> GetHitCounts(bool reset);
    std::optional<glm::vec3> GetLastHit();
    // whether an actor's firing loop sounds now (the Death Machine's), for the harness. Main thread only.
    bool IsFireLoopPlaying(std::uint32_t id);
    // puts every stowed gun on the rig a body without the tag gets, to test the rig on a body that can compare it with
    // the tag, for the harness
    void SetStowedRigForced(bool on);
    // the rockets in the air, each by its model and where it is, for the harness. Main thread only.
    std::vector<std::pair<std::string, glm::vec3>> GetRockets();
    // the models an actor is built of, by name with the tag each hangs on, for the harness. Main thread only.
    std::vector<std::string> GetBuiltModelTags(std::uint32_t id);

    // Where an actor's bones are as it was last drawn, for the harness; nullopt for a bone it doesn't have, empty when
    // the actor isn't built. Main thread only.
    std::vector<std::optional<glm::vec3>> GetBonePositions(std::uint32_t id, const std::vector<std::string>& bones);
    std::vector<std::optional<glm::mat4>> GetBoneMatrices(std::uint32_t id, const std::vector<std::string>& bones);
    // its model's transform in the world as it was last drawn, none when it isn't built
    std::optional<glm::mat4> GetDrawnPlacement(std::uint32_t id);

    // For the bone camera on an actor: where a bone of an actor is as it was last drawn
    // (in this frame, once the actors were added to its scene), with where it stands and faces; none while it isn't
    // built, shown or placed, or hasn't the bone. Main thread.
    std::optional<Types::TargetBone> GetDrawnBone(std::uint32_t id, const std::string& bone);
    // The names of the bones of an actor's DObj (each of its models', once), empty when it isn't built. Main thread.
    std::vector<std::string> GetBuiltBones(std::uint32_t id);
    std::string DescribeAnimationBones(const std::string& animation);

    // Where an actor's magazines were last drawn, for the harness: whether the gun's own is hidden, the one in a hand
    // and those on the ground. Main thread only.
    struct MagazineState
    {
        std::string tag;                        // the magazine bone
        bool offGun = false;                    // the actor was last drawn with the gun's magazine hidden
        std::string held;                       // "old" or "new" when it was drawn with a magazine in a hand
        std::optional<glm::mat4> heldMagazine;  // where that one's magazine bone was drawn
        std::optional<float> heldToGun;         // how far that is from the gun's magazine bone
        std::vector<float> dropped;  // how high the middle of each magazine on the ground (the latest first) is above
                                     // the ground below it
        std::vector<float> droppedFromActor;       // and how far it is from where the actor stands, along the ground
        std::optional<glm::mat4> droppedMagazine;  // where the latest one's magazine bone was drawn
        std::string reload;  // the last reload's ticks (out, swap, in and end) and the hand's bone
    };
    std::optional<MagazineState> GetMagazineState(std::uint32_t id);

    // What an actor's Fire events drop as the game shows it, for the harness: where
    // each thing left from and landed, where its model was last drawn (none while it isn't), and how many of its
    // effects there are and have begun. Main thread only.
    struct DropState
    {
        std::uint32_t event = 0;
        std::uint32_t shot = 0;
        std::string model;
        std::uint32_t releaseTick = 0;
        glm::vec3 release{};
        std::optional<glm::vec3> landing;
        float seconds = 0.0f;
        std::optional<glm::vec3> drawnAt;
        std::size_t plays = 0;
        std::size_t begun = 0;
    };
    std::vector<DropState> GetDropStates(std::uint32_t id);
    // Where a prop on its own was last drawn, none while it isn't; and how many models on their own are drawn, for the
    // harness. Main thread only.
    std::optional<glm::vec3> GetPlacedPropDrawn(std::uint32_t id);
    std::size_t CountSceneModels();
    // Where the prop browser's preview was last drawn, none while it isn't
    std::optional<glm::vec3> GetPropPreviewDrawn();

    // Where the models on an actor's bones were last drawn, and how far each is from where its bone and offset put it,
    // for the harness; empty when the actor isn't built. Main thread only.
    struct PropState
    {
        std::string model;
        bool drawn = false;
        glm::vec3 position{};
        float distance = 0.0f;
    };
    std::vector<PropState> GetPropStates(std::uint32_t id);

    // Turns the bones of actors the game's controllers turn (see Actors.cpp): the heads of those that look at
    // something, killstreaks' bodies, steering and wheels; from CG_DoControllers on any thread
    void ControlJoints(Structures::DObj* obj, int* partBits);
    // Turns the heads of actors that look at something from where the animations have posed them, as DObjCalcAnim
    // works out the bones of partBits that it hadn't (worked: obj's animPartBits before it); on any thread
    void BlendHeads(Structures::DObj* obj, const int* partBits, const std::array<std::uint32_t, 5>& worked);

    // The left hand on the gun (GripLogic). AskGripBones, as CG_DoControllers starts, has an
    // actor's whole skeleton worked out at once, as the arm needs the bones of both arms; GripGuns, once DObjCalcSkel has
    // put it together (composed: the bones it had before), bends the arm. Both run on the threads that work out
    // skeletons.
    void AskGripBones(const Structures::DObj* obj, int* partBits);
    void GripGuns(Structures::DObj* obj, const std::array<std::uint32_t, 5>& composed);
    // for the harness: without the grip, as the animations have the hand
    void SetGripsOn(bool on);
    // for the harness: without going to a place on animations made for the AI (see GripLogic::GetCampaignShare)
    void SetGripPlaces(bool on);
    // For the harness: turns a bone of an actor as the game's controllers do (its rotation relative to its parent in
    // place of its own, the game's angles), after what its killstreak's body turns; none to stop. Main thread only.
    void SetJointOverride(std::uint32_t id, const std::string& bone, std::optional<glm::vec3> angles);

    // How many degrees the face of an actor's head is off from what it looks at now, for the harness; none when it
    // looks at nothing. Main thread only.
    std::optional<float> GetLookError(std::uint32_t id);

    // How the game's delta of the animation of one of an actor's clips (XAnimGetRelDelta, which root motion is measured
    // with) moves the root from one point of a play to another (0 to 1), for the harness: the translation (x forward,
    // y to the left of where it faced at the first point, z up) and the rotation about z as the game gives it; none
    // when the actor isn't built of that clip or its animation has no delta. Main thread only.
    struct AnimationDelta
    {
        glm::vec3 translation{0.0f};
        float rotation[2] = {};
    };
    std::optional<AnimationDelta> GetAnimationDelta(std::uint32_t id, std::uint32_t clipId, float from, float to);

    // the models of an actor's DObj (its body's spine rig first, if it has one), empty when it isn't built, for the
    // harness. Main thread only.
    std::vector<std::string> GetBuiltModels(std::uint32_t id);
    // the materials of the surfaces an actor's body model draws at its first level of detail (the one the free camera
    // draws), for the harness. Main thread only.
    std::vector<std::string> GetFirstLodMaterials(std::uint32_t id);
    // the animations of the store (see AnimationStore) an actor's tree has, for the harness. Main thread only.
    std::vector<std::string> GetStoredAnimations(std::uint32_t id);

    // What an actor's animation tree played as it was last drawn, for the harness: the class of its gun as far as its
    // legs go and the weapon def's fields that tell it, the legs' idles of the tree, the weight each entry was last
    // given (by its animation's name; lined and resting are the spine's, see SpineRig in Actors.cpp), and whether its
    // legs are left out. None when it isn't built. Main thread only.
    struct TreeState
    {
        Types::GunClass gunClass = Types::GunClass::Rifle;
        ActorLogic::Body body;             // what its legs stand and walk as
        std::int32_t playerAnimType = -1;  // -1 without a gun
        std::int32_t weapClass = -1;
        std::vector<std::string> legs;
        std::vector<std::string> faces;  // the face's entries, and the weights they were given last
        std::vector<std::pair<std::string, float>> faceWeights;
        std::vector<std::pair<std::string, float>> weights;
        bool legsLeftOut = false;
    };
    std::optional<TreeState> GetTreeState(std::uint32_t id);
    // What an actor's spine was drawn with as it was last drawn, for the harness (see SpineRig in Actors.cpp): whether
    // its DObj has a torso_stabilizer (an MP body's or its spine rig's), how much its entries of animations made for MP
    // bodies and of those made for SP bodies (that line the stabilizer up) weighed, its clips' and legs' alike, and the
    // made up entries lining the stabilizer up and resting it (none when it has none). None when it isn't built. Main
    // thread only.
    struct SpineState
    {
        bool stabilizer = false;
        bool rig = false;
        float mpWeight = 0.0f;
        float spWeight = 0.0f;
        std::optional<float> lined;
        std::optional<float> resting;
        std::optional<float> aimed;  // under upper body clips over legs made for SP bodies (see PoseLogic::AimSpine)
    };
    std::optional<SpineState> GetSpineState(std::uint32_t id);
    // Draws an actor without legs under its upper body clips, as before there were any, or with them again; for the
    // harness to check such a clip's decoded pose alone (CheckDecodedPose). Main thread only.
    void SetLegsLeftOut(std::uint32_t id, bool leftOut);

    // How far the decoded animation data puts an actor's spine, hands and gun (GameInterface::GetAnimationPoseByName,
    // and GetWeaponRig for the gun's magazine) from where the game has them as it was last drawn, for the harness;
    // none when it isn't built. Decoded that much later in the animation (0 to 1), it shows how far they move
    // meanwhile. Main thread only.
    struct DecodedPoseCheck
    {
        std::string animation;  // of the clip it shows
        float time = 0.0f;      // in the animation, 0 to 1
        float weight = 0.0f;    // the clip's, which is checked alone
        std::string spine;      // what the animation does with an MP body's spine (see PoseLogic::SpineKind)
        struct Bone
        {
            std::string name;
            // how far apart the transforms are (see PoseLogic::GetTransformDistance), none when the decoder or the
            // game doesn't have the bone
            std::optional<float> distance;
            float origins = 0.0f;  // how far apart their origins are
        };
        std::vector<Bone> bones;
        // how far the rig's magazine is from where the game has it relative to tag_weapon_right (the gun's root)
        std::optional<float> rigToGame;
        std::string error;  // why it couldn't be checked
    };
    std::optional<DecodedPoseCheck> CheckDecodedPose(std::uint32_t id, float later = 0.0f);

    // Builds every character the Actors tab offers the way actors are built (each body with no head or each head,
    // and with nothing or each thing worn, without a gun and with the gun of the most bones, and an SP body with the
    // spine rig it gets for animations made for MP bodies), for the harness: how
    // many have more bones than a DObj can, and whether what is built of them still has (overBuilt) or lacks a body
    // that fits alone (bodyLeftOut). Up to maxBuilt of those over the limit are made into DObjs and freed again, so
    // the game counts them itself. Main thread only.
    struct BoneCheck
    {
        std::size_t characters = 0;  // with and without the gun
        std::size_t overLimit = 0;
        std::size_t overBuilt = 0;
        std::size_t bodyLeftOut = 0;
        std::size_t built = 0;
        std::string gun;                    // its model and bones
        std::vector<std::string> examples;  // of those over the limit
    };
    BoneCheck CheckBones(std::size_t maxBuilt);

    // The ground where a ray from the origin hits the world: what it hits, or where it ends, and from there down
    std::optional<glm::vec3> FindGround(glm::vec3 origin, glm::vec3 direction);

    // Another map: what actors were built of is gone. Main thread. (A demo's load does this; a live game's new map
    // too.)
    void ResetForMap();

    void Install();
}  // namespace IWXMVM::T5::Actors
