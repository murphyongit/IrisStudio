#pragma once

#include <glm/glm.hpp>

namespace IWXMVM::Types
{
    // What a reload animation does with the gun's magazine, worked out once for an animation, a body and a gun (see
    // ReloadLogic): when the magazine leaves the gun, when the hand swaps it for a new one and when that one goes in,
    // the hand that holds it and how, and where the old one falls from. A clip only maps it onto the timeline.

    enum class ReloadKind
    {
        None,             // nothing to move: the gun has no magazine, or nothing tells when it would move
        Magazine,         // the hand pulls the magazine out, swaps it for a new one (at a pouch) and puts that in
        DroppedMagazine,  // the magazine falls from the gun, and the hand brings a new one
        Box,              // an LMG's box and belt (not drawn yet)
        Shells,           // a shotgun's shells, one at a time (not drawn yet)
        Rocket,           // a launcher's rocket (not drawn yet)
    };

    // the bone a magazine is held with
    enum class ReloadHand
    {
        HandTag,  // tag_inhand, which some SP animations move with a hand, as their scripts hold clip models on it
        LeftWrist,
        RightWrist,
    };

    // where a part of a profile comes from: the first of these that says
    enum class ReloadSource
    {
        None,
        Override,         // the user's, saved in config.json
        Curated,          // the table that comes with the mod (resources/reload-profiles.json)
        ClipNotetracks,   // SP animations' drop clip, attach clip and detach clip, as SP scripts use them
        SoundNotetracks,  // the sounds of MP animations: magazine out and in, shells, the LMG's box
        Geometry,         // where the hand goes: from the magazine, away to a pouch, and back
    };

    struct ReloadMoment
    {
        float time = 0.0f;  // of a play of the animation, from 0 to 1
        ReloadSource source = ReloadSource::None;

        bool operator==(const ReloadMoment&) const = default;
    };

    // How a hand holds a magazine: where the gun's magazine bone is relative to the hand's bone
    struct ReloadGrip
    {
        enum class Kind
        {
            Authored,  // exactly on tag_inhand, as SP scripts hold their clip models (when that is where the gun has
                       // it)
            Measured,  // as the hand is to the magazine on the gun at the moment it takes it out or puts it in
        };
        Kind kind = Kind::Measured;
        glm::mat4 magazine{1.0f};
    };

    // Where the old magazine starts to fall: from the gun as it leaves it, or from the hand as it swaps it
    struct ReloadFall
    {
        float time = 0.0f;         // of a play, from 0 to 1
        glm::mat4 magazine{1.0f};  // the magazine bone's transform in the model then
        glm::vec3 velocity{0.0f};  // of its middle, in the model, units a second as the animation plays at 1x
        glm::vec3 push{0.0f};      // and from the gun pushing it out, units a second, however fast it plays
        float spin = 0.0f;         // degrees a second, about its thin side
        bool fromGun = false;
    };

    struct ReloadProfile
    {
        ReloadKind kind = ReloadKind::None;
        ReloadSource kindSource = ReloadSource::None;
        ReloadHand hand = ReloadHand::LeftWrist;
        ReloadSource handSource = ReloadSource::None;
        std::optional<ReloadMoment> out;   // the old magazine leaves the gun (into the hand, or falling)
        std::optional<ReloadMoment> swap;  // the hand has the new one from here (and drops the old one it pulled)
        std::optional<ReloadMoment> in;    // the new one is on the gun from here
        std::vector<float> inserts;        // Shells: each shell going in, from 0 to 1
        // how the hand holds the old magazine from out to the swap (a pulled one), and the new one until in; none when
        // they can't be worked out (the game can't decode the animation's poses, or the gun has no magazine)
        std::optional<ReloadGrip> oldGrip;
        std::optional<ReloadGrip> newGrip;
        std::optional<ReloadFall> fall;
        std::string magazineBone;  // the gun's (see WeaponRig)
        std::string note;          // what couldn't be worked out, and why, for the UI
    };
}  // namespace IWXMVM::Types
