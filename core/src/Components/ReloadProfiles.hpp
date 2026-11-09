#pragma once
#include "Types/Actor.hpp"
#include "Types/ReloadProfile.hpp"
#include "Components/ActorLogic.hpp"
#include "Components/ReloadLogic.hpp"

namespace IWXMVM::Components::ReloadProfiles
{
    // The reload profiles (see ReloadLogic) of the animations actors play, each worked out once for an animation, a
    // body and a gun from what the game tells of them (its notetracks in the actors' catalog,
    // GameInterface::GetAnimationPoseByName and GetWeaponRig), on the mod's own thread rather than the game's, and kept
    // until the catalog changes (another map or pack). What the user says of an animation is kept in config.json; the
    // curated table comes with the mod (resources/reload-profiles.json).

    void Initialize();
    // Works out the profiles asked for since; the mod's own thread calls it over and over
    void Work();

    // An animation's profile for a body model and a gun (a weapon variant, e.g. ak47_mp; empty for none). Null until it
    // is worked out: the first call asks for it. While it is worked out again after its override changed, the one
    // worked out last.
    std::shared_ptr<const Types::ReloadProfile> Get(const std::string& animation, const std::string& body,
                                                    const std::string& weapon);
    // whether Get gives the profile as the animation's override is now
    bool IsUpToDate(const std::string& animation, const std::string& body, const std::string& weapon);
    // changes whenever profiles worked out before no longer hold (another catalog, an override)
    std::uint32_t GetGeneration();
    // how many profiles were worked out since the mod started, for the harness
    std::uint32_t CountWorkedOut();

    // The bones a reload needs from the game's decoded poses of an animation on a body (GetAnimationPoseByName)
    ReloadLogic::BonesAt GetBones(const std::string& animation, const std::string& body);

    // The profiles of an actor's clips' animations, for its body and the gun it holds
    ReloadLogic::ProfileLookup GetLookup(const Types::Actor& actor);
    // The reloads of an actor's clips up to a tick (see ActorLogic::GetReloads) as their profiles say; a clip whose
    // profile isn't worked out yet, or moves no magazine the game can draw, has none (its magazine stays on the gun)
    std::vector<ActorLogic::Reload> GetReloads(const Types::Actor& actor, std::uint32_t until);

    // What the user says of an animation, which comes before anything else (see ReloadLogic::ProfileEntry), as it is
    // used now (previewed or saved); none (or one that says nothing) clears it. Set saves it to config.json at once.
    std::optional<ReloadLogic::ProfileEntry> GetOverride(const std::string& animation);
    void SetOverride(const std::string& animation, const std::optional<ReloadLogic::ProfileEntry>& entry);
    // Sets it as the user changes it: one step of the editor's undo history, named for the Edit menu ("move swap of
    // exposed_reload"), unless it is what config.json has already
    void ChangeOverride(const std::string& animation, const std::optional<ReloadLogic::ProfileEntry>& entry,
                        std::string label);
    // Uses it without saving it or adding a step, while a marker is dragged; ChangeOverride ends that (with what
    // config.json has to put it back)
    void PreviewOverride(const std::string& animation, const std::optional<ReloadLogic::ProfileEntry>& entry);
}  // namespace IWXMVM::Components::ReloadProfiles
