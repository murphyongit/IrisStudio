#pragma once
#include "Types/Actor.hpp"

namespace IWXMVM::Types
{
    // Where an animation stands, sits or lies, and how it moves, as it starts or ends
    enum class AnimationPosture
    {
        Unknown,
        Stand,
        Crouch,
        Prone,
        Back,  // lying on the back
    };
    enum class AnimationMovement
    {
        Unknown,
        Stop,
        Walk,
        Run,
        Sprint,
    };
    struct AnimationState
    {
        AnimationPosture posture = AnimationPosture::Unknown;
        AnimationMovement movement = AnimationMovement::Unknown;

        bool operator==(const AnimationState&) const = default;
    };

    // A zone of the game animations come from, e.g. khe_sanh, "S.O.G.", "Campaign"
    struct AnimationSource
    {
        std::string zone;
        std::string name;   // what it is called: a level's or map's name, else its group's
        std::string group;  // Multiplayer, Campaign, Zombies, Multiplayer maps
    };

    // Where a foot comes down in a play of a moving animation
    struct AnimationFootfall
    {
        float time = 0.0f;  // from 0 (the start) to 1 (the end)
        bool left = false;

        bool operator==(const AnimationFootfall&) const = default;
    };

    // An animation of the game, as the catalog of every animation the game has lists it (resources/animations.json,
    // which tools/t5-characters/animations.py writes from the game's zones)
    struct AnimationEntry
    {
        // the animation's name, or for a copy whose data differs from the one of that name, the name and the zone it
        // is from: "pb_stand_alert_pistol (common_zombie)"
        std::string name;
        std::vector<std::uint16_t> sources;  // the zones it is in (AnimationCatalog::sources), where it is from first
        std::uint32_t frames = 0;            // after the first one: 0 for a pose
        float rate = 30.0f;                  // frames a second
        bool loop = false;                   // made to loop, such as idles and walks
        bool delta = false;                  // moves the root (j_mainroot's motion goes to the entity)
        AnimationKind kind = AnimationKind::Other;
        AnimationFamily family = AnimationFamily::Other;
        std::uint32_t boneSet = 0;               // the bones it has tracks for (AnimationCatalog::boneSets)
        std::vector<ActorNotetrack> notetracks;  // sorted by time
        std::uint32_t bytes = 0;                 // what it takes in memory
        std::string hash;                        // of its data, as the animation pack has it
        glm::vec3 motion{0.0f};                  // how far its root moves in a play: x forward, y left, z up
        float turn = 0.0f;                       // how far it turns in a play, degrees to the left
        AnimationState from;
        AnimationState to;
        // where a moving loop without footstep notetracks comes down on its feet, in order, as worked out from its
        // legs (see tools/t5-characters/animations.py's footfalls)
        std::vector<AnimationFootfall> footfalls;
        // whether it is a death (see ActorLogic::IsDeathAnimation) and aims or shoots (ActorLogic::IsAlertAnimation),
        // worked out from its name and notetracks as the catalog is read
        bool death = false;
        bool alert = false;

        float Duration() const
        {
            return rate > 0.0f ? static_cast<float>(frames) / rate : 0.0f;
        }

        // the name the game has it by (the name without the zone of a differing copy)
        std::string_view AssetName() const
        {
            const std::string_view view = name;
            return view.substr(0, view.find(' '));
        }
    };

    struct AnimationCatalog
    {
        std::vector<AnimationSource> sources;  // in the order they are listed (multiplayer's first)
        std::vector<std::string> bones;
        std::vector<std::vector<std::uint32_t>> boneSets;  // of indices into bones
        std::vector<AnimationEntry> entries;               // sorted by name

        const AnimationEntry* Find(std::string_view name) const
        {
            const auto it = std::ranges::lower_bound(entries, name, {}, &AnimationEntry::name);
            return it != entries.end() && it->name == name ? &*it : nullptr;
        }

        // where an animation is listed: the first of its sources
        const AnimationSource* GetSource(const AnimationEntry& entry) const
        {
            return entry.sources.empty() || entry.sources.front() >= sources.size() ? nullptr
                                                                                   : &sources[entry.sources.front()];
        }
    };
}  // namespace IWXMVM::Types
