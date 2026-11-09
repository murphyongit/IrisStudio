#pragma once
#include "Types/Actor.hpp"
#include "Types/ReloadProfile.hpp"
#include "Types/WeaponRig.hpp"
#include "Components/ActorLogic.hpp"

#include <span>

#include "nlohmann/json.hpp"

namespace IWXMVM::ReloadLogic
{
    // Reload profiles (see Types::ReloadProfile) worked out from a reload animation alone: what the user and the
    // curated table say of it, its notetracks, and where its bones go, with no game, so the tests in tests/actors-tests
    // build it on its own. Components::ReloadProfiles asks the game for the bones and keeps the profiles. Each part
    // comes from the first source that has it: the user's override, the curated table, SP clip notetracks, sound
    // notetracks, then geometry (where the hand goes); else there is none.

    // Where the bones a reload needs are at a time of the animation, in the model (see PoseLogic)
    struct ReloadBones
    {
        glm::mat4 leftWrist{1.0f};
        glm::mat4 rightWrist{1.0f};
        std::optional<glm::mat4> handTag;  // tag_inhand, for a body that has it
        glm::mat4 gun{1.0f};               // tag_weapon_right, which the gun hangs from
    };
    // the bones at a time from 0 to 1; none where the game can't tell (none for reloads of 256 frames or more, see
    // ReloadProfiles)
    using BonesAt = std::function<std::optional<ReloadBones>(float time)>;

    // What the user or the curated table say of a reload animation: what each gives takes the place of what would be
    // worked out
    struct ProfileEntry
    {
        std::optional<Types::ReloadKind> kind;
        std::optional<Types::ReloadHand> hand;
        std::optional<float> out;
        std::optional<float> swap;
        std::optional<float> in;

        bool operator==(const ProfileEntry&) const = default;
    };
    // {"kind": "magazine", "hand": "left", "out": 0.014, "swap": 0.32, "in": 0.42}, each optional; kinds none,
    // magazine, dropped, box, shells, rocket; hands tag, left, right. Unknown names are left out.
    nlohmann::json EntryToJson(const ProfileEntry& entry);
    ProfileEntry EntryFromJson(const nlohmann::json& json);
    // a table of them by animation, {"profiles": {"pt_reload_stand_auto": {...}, ...}}; empty for anything else
    using ProfileTable = std::map<std::string, ProfileEntry, std::less<>>;
    ProfileTable TableFromJson(const nlohmann::json& root);

    std::string_view GetKindName(Types::ReloadKind kind);
    std::string_view GetHandName(Types::ReloadHand hand);
    std::string_view GetSourceName(Types::ReloadSource source);

    // The moments of a profile, as the UI shows them to move: when the magazine leaves the gun, when the hand has the
    // new one and when that one is in
    enum class Moment
    {
        Out,
        Swap,
        In,
    };
    inline constexpr std::array<Moment, 3> MOMENTS{Moment::Out, Moment::Swap, Moment::In};
    // "out", "swap", "in"
    std::string_view GetMomentName(Moment moment);
    const std::optional<Types::ReloadMoment>& GetMoment(const Types::ReloadProfile& profile, Moment moment);
    std::optional<float>& GetMoment(ProfileEntry& entry, Moment moment);
    std::optional<float> GetMoment(const ProfileEntry& entry, Moment moment);
    // A moment of a profile moved to a time: within 0 to 1, in order with the profile's other moments (out, swap, in)
    // and at least MIN_MOMENT_GAP of a play from them, so a profile worked out with it there keeps them all
    inline constexpr float MIN_MOMENT_GAP = 0.005f;
    float PlaceMoment(const Types::ReloadProfile& profile, Moment moment, float time);
    // An override with a kind, none to work it out again. A magazine for an animation whose profile has no out or in
    // (nothing told when) starts with its moments at DEFAULT_MOMENTS, to move from there.
    inline constexpr std::array<float, 3> DEFAULT_MOMENTS{0.15f, 0.45f, 0.75f};
    ProfileEntry WithKind(ProfileEntry entry, std::optional<Types::ReloadKind> kind,
                          const Types::ReloadProfile& profile);
    // whether a profile says anything of a reload: a kind or a moment from any source (an animation that isn't a
    // reload has none)
    bool SaysReload(const Types::ReloadProfile& profile);

    // What a profile is worked out from. Only a reload gets one: an animation named so (IsReloadName), or one whose
    // notetracks, the curated table or the user's override say is one; never a death unless the user says so (the hand
    // of a death flails by the magazine).
    struct ProfileInput
    {
        std::string_view animation;                         // its name
        bool death = false;                                 // a death (see ActorLogic::IsDeathAnimation)
        float duration = 0.0f;                              // seconds
        std::span<const Types::ActorNotetrack> notetracks;  // sorted by time
        const ProfileEntry* override = nullptr;
        const ProfileEntry* curated = nullptr;
        std::optional<Types::WeaponRig> rig;  // of the gun in the hand; none for none
        BonesAt bones;                        // empty when the game can't tell
    };
    Types::ReloadProfile MakeProfile(const ProfileInput& input);
    // whether an animation's name says it is a reload ("reload" in it)
    bool IsReloadName(std::string_view animation);

    // The moments the notetracks of an animation give (see Types::ReloadSource): SP clip notetracks ("drop clip",
    // "attach clip left|right", "detach clip ...", "reload done"), or the sounds of MP animations by class: magazine
    // out and in (rifles, SMGs, pistols), the LMG's box (open or box off, then the belt placed), a shotgun's shells (MP
    // sounds or SP "load_shell") and a launcher's rocket; none when they say nothing of a reload
    struct NotetrackMoments
    {
        Types::ReloadKind kind = Types::ReloadKind::None;
        Types::ReloadSource source = Types::ReloadSource::None;
        std::optional<float> out;
        std::optional<float> swap;
        std::optional<float> in;
        std::vector<float> inserts;
    };
    std::optional<NotetrackMoments> FindClipMoments(std::span<const Types::ActorNotetrack> notetracks);
    std::optional<NotetrackMoments> FindSoundMoments(std::span<const Types::ActorNotetrack> notetracks);

    // How far the hand is from the magazine on the gun over an animation, at times from 0 to 1 in order
    struct Curve
    {
        std::vector<float> times;
        std::vector<float> distances;
    };
    // the farthest point of the curve strictly between two times, none when no sample is between them
    std::optional<float> FindFarthest(const Curve& curve, float from, float to);
    // A trip of the hand to fetch a magazine: it leaves the magazine, goes away (to a pouch) and comes back. The hand
    // goes farthest at the swap, which is at least MIN_TRIP away; out is the last point it is nearest the magazine
    // before (within NEAR_MAGAZINE), in the first one after. There it barely moves relative to the magazine, so the
    // magazine doesn't jump as it changes from the gun to the hand however a clip's ticks fall. None when the hand
    // makes no such trip.
    struct Trip
    {
        float out = 0.0f;
        float swap = 0.0f;
        float in = 0.0f;
    };
    inline constexpr float NEAR_MAGAZINE = 10.0f;  // units from the wrist to the magazine bone
    inline constexpr float MIN_TRIP = 15.0f;
    std::optional<Trip> FindTrip(const Curve& curve);

    // a hand bone of the bones, none for a tag_inhand the body hasn't
    std::optional<glm::mat4> GetHand(const ReloadBones& bones, Types::ReloadHand hand);
    // where the gun's magazine bone is in the model
    glm::mat4 GetMagazineOnGun(const ReloadBones& bones, const Types::WeaponRig& rig);

    // Whether tag_inhand moves with a wrist over a stretch of the animation, as it does in SP animations that hold a
    // clip model on it: for most of the stretch it stays where it is at its middle relative to the wrist, within a
    // hand's length of it
    bool MovesWith(std::span<const ReloadBones> stretch, Types::ReloadHand wrist);

    // The magazine model's transform (a gun model drawn with only its magazine, as games draw a magazine apart from the
    // gun) from where its magazine bone is
    glm::mat4 GetMagazineModel(const glm::mat4& magazineBone, const Types::WeaponRig& rig);
    // The magazine's middle in the gun model: a little below its magazine bone
    inline constexpr glm::vec3 MAGAZINE_MIDDLE{0.0f, 0.0f, -3.5f};
    inline constexpr float MAX_MAGAZINE_SPEED = 150.0f;  // units a second, of the hand's motion it falls with
    inline constexpr float MAGAZINE_PUSH = 40.0f;        // how fast a gun lets a magazine go, downwards out of it

    // How a profile's old magazine falls (see ActorLogic::MagazineFall), for an actor standing at a placement (its
    // model's transform in the world) and playing the animation at a speed; groundBelow gives the height of the
    // ground below a point. It starts where the profile has it, from the animation alone, or where a game draws the
    // magazine bone then (drawnMagazine, in the world: from where it draws the gun or the hand it falls from, which
    // the legs under an upper body clip move), its motion turned with it. None for a profile whose magazine doesn't
    // fall.
    std::optional<ActorLogic::MagazineFall> PlaceFall(const Types::ReloadProfile& profile, const Types::WeaponRig& rig,
                                                      const glm::mat4& placement, float speed,
                                                      const std::function<float(glm::vec3 point)>& groundBelow,
                                                      const std::optional<glm::mat4>& drawnMagazine = std::nullopt);

    // The moments of a profile a clip maps to its ticks (see ActorLogic::GetReloads); none for a kind that isn't drawn
    // or one without an out and an in
    std::optional<ActorLogic::ReloadMoments> GetMoments(const Types::ReloadProfile& profile);

    // the bone of a hand: tag_inhand, j_wrist_le or j_wrist_ri
    std::string_view GetHandBone(Types::ReloadHand hand);

    // What an actor's magazines do at a tick, from its reloads by then (ActorLogic::GetReloads with its profiles'
    // moments, see Components::ReloadProfiles::GetReloads) and their profiles, by the reload's clip: whether the gun's
    // own is off it, the one in the hand and how that holds it (the old one with the grip it took it out with until
    // the swap, the new one with the grip it puts it in with), and the last few that fell, the latest first. Only the
    // last reload may still be playing: a reload stops where the next starts. A game draws the one in the hand on the
    // hand's bone as it is now, and places those that fell where the actor was as they started to fall (PlaceFall).
    struct Magazines
    {
        ActorLogic::ReloadState state;                          // of the last reload
        Types::ReloadHand hand = Types::ReloadHand::LeftWrist;  // its profile's
        glm::mat4 held{1.0f};  // the magazine model's transform (see GetMagazineModel) relative to the hand's bone
        struct Fallen
        {
            const ActorLogic::Reload* reload = nullptr;  // points into the reloads
            std::shared_ptr<const Types::ReloadProfile> profile;
            std::uint32_t start = 0;  // the tick it started to fall
        };
        std::vector<Fallen> fallen;
    };
    // the profile of a clip's animation, null when there is none (yet)
    using ProfileLookup = std::function<std::shared_ptr<const Types::ReloadProfile>(const Types::ActorClip& clip)>;
    Magazines GetMagazines(std::span<const ActorLogic::Reload> reloads, const ProfileLookup& profiles,
                           const Types::WeaponRig& rig, std::uint32_t tick, std::size_t maxFallen);
}  // namespace IWXMVM::ReloadLogic
