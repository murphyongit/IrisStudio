#include "StdInclude.hpp"
#include "AnimationCatalogLogic.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/PoseLogic.hpp"

#include <numeric>

namespace IWXMVM::AnimationCatalogLogic
{
    namespace
    {
        using Kind = Types::AnimationKind;
        using Family = Types::AnimationFamily;
        using Posture = Types::AnimationPosture;
        using Movement = Types::AnimationMovement;

        Kind ParseKind(std::string_view kind)
        {
            if (kind == "FullBody")
                return Kind::FullBody;
            if (kind == "Torso")
                return Kind::Torso;
            if (kind == "Face")
                return Kind::Face;
            return Kind::Other;
        }

        Family ParseFamily(std::string_view family)
        {
            if (family == "MpPlayer")
                return Family::MpPlayer;
            if (family == "SpHuman")
                return Family::SpHuman;
            if (family == "Human")
                return Family::Human;
            if (family == "Dog")
                return Family::Dog;
            if (family == "Viewmodel")
                return Family::Viewmodel;
            return Family::Other;
        }

        Posture ParsePosture(const nlohmann::json& value)
        {
            if (!value.is_string())
                return Posture::Unknown;
            const auto posture = value.get<std::string>();
            if (posture == "stand")
                return Posture::Stand;
            if (posture == "crouch")
                return Posture::Crouch;
            if (posture == "prone")
                return Posture::Prone;
            if (posture == "back")
                return Posture::Back;
            return Posture::Unknown;
        }

        Movement ParseMovement(const nlohmann::json& value)
        {
            if (!value.is_string())
                return Movement::Unknown;
            const auto movement = value.get<std::string>();
            if (movement == "stop")
                return Movement::Stop;
            if (movement == "walk")
                return Movement::Walk;
            if (movement == "run")
                return Movement::Run;
            if (movement == "sprint")
                return Movement::Sprint;
            return Movement::Unknown;
        }

        // the bones of a human's rig, by the start of their names: the skeleton every body has (give or take a helper
        // bone), not clothes, gear or what is carried
        constexpr std::array<std::string_view, 20> HUMAN_BONES = {
            "j_mainroot", "pelvis",      "torso_stabilizer", "j_spine",   "j_neck",  "j_head",   "j_hip_",
            "j_hiptwist_", "j_knee_",    "j_ankle_",         "j_ball_",   "j_clavicle_", "j_shoulder", "j_elbow_",
            "j_wrist",    "j_index_",    "j_mid_",           "j_ring_",   "j_pinky_", "j_thumb_"};
        // a dog's: its joints and ears
        constexpr std::array<std::string_view, 3> DOG_JOINTS = {"j_", "le_ear", "ri_ear"};

        // how an animation is fitted: which rig a model must have, and which of its bones count
        enum class FitClass
        {
            Human,
            Dog,
            Face,
            Other,
        };

        FitClass GetFitClass(const Types::AnimationEntry& entry)
        {
            if (entry.kind == Kind::Face)
                return FitClass::Face;
            switch (entry.family)
            {
                case Family::MpPlayer:
                case Family::SpHuman:
                case Family::Human:
                    return FitClass::Human;
                case Family::Dog:
                    return FitClass::Dog;
                default:
                    return FitClass::Other;
            }
        }

        // how many of the bones an animation has tracks for count for its rig (all of them for a prop's, or when none
        // of the rig's do), and how many of those the model has
        struct BoneCount
        {
            std::size_t total = 0;
            std::size_t present = 0;
        };
        BoneCount CountBones(const Types::AnimationCatalog& catalog, std::uint32_t boneSet, FitClass fitClass,
                             const ModelRig& rig)
        {
            const auto counted = fitClass == FitClass::Human ? std::span<const std::string_view>(HUMAN_BONES)
                                 : fitClass == FitClass::Dog ? std::span<const std::string_view>(DOG_JOINTS)
                                                             : std::span<const std::string_view>();
            const auto count = [&](bool all) {
                BoneCount bones;
                for (const auto index : catalog.boneSets[boneSet])
                {
                    if (index >= catalog.bones.size())
                        continue;
                    const auto& bone = catalog.bones[index];
                    if (!all && !PoseLogic::StartsWithAny(bone, counted))
                        continue;
                    ++bones.total;
                    bones.present += rig.bones.contains(bone) ? 1 : 0;
                }
                return bones;
            };
            const auto bones = count(counted.empty());
            // an animation of none of the rig's bones counts all of its own
            return bones.total == 0 ? count(true) : bones;
        }

        bool HasBones(const Types::AnimationCatalog& catalog, std::uint32_t boneSet)
        {
            return boneSet < catalog.boneSets.size() && !catalog.boneSets[boneSet].empty();
        }

        bool IsShareEnough(const BoneCount& bones)
        {
            return bones.total > 0 &&
                   static_cast<float>(bones.present) >= FIT_SHARE * static_cast<float>(bones.total) - 1e-4f;
        }

        bool FitsBones(const Types::AnimationCatalog& catalog, std::uint32_t boneSet, FitClass fitClass,
                       const ModelRig& rig)
        {
            if (!HasBones(catalog, boneSet))
                return false;
            switch (fitClass)
            {
                case FitClass::Face:
                    return rig.face;
                case FitClass::Human:
                    if (!rig.human)
                        return false;
                    break;
                case FitClass::Dog:
                    if (!rig.dog)
                        return false;
                    break;
                case FitClass::Other:
                    break;
            }
            return IsShareEnough(CountBones(catalog, boneSet, fitClass, rig));
        }

        std::string Lower(std::string_view text)
        {
            std::string out(text);
            std::ranges::transform(out, out.begin(), [](char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        bool IsMoving(Movement movement)
        {
            return movement == Movement::Walk || movement == Movement::Run || movement == Movement::Sprint;
        }
    }  // namespace

    Types::AnimationCatalog Parse(const nlohmann::json& root)
    {
        if (root.value("version", 0) != 1)
            throw std::runtime_error("animations.json isn't of version 1");
        Types::AnimationCatalog catalog;
        for (const auto& zone : root.at("zones"))
            catalog.sources.push_back(
                {zone.at("zone").get<std::string>(), zone.value("name", ""), zone.value("group", "")});
        catalog.bones = root.at("bones").get<std::vector<std::string>>();
        for (const auto& set : root.at("boneSets"))
            catalog.boneSets.push_back(set.get<std::vector<std::uint32_t>>());
        const auto notetracks = root.at("notetracks").get<std::vector<std::string>>();

        const auto& animations = root.at("animations");
        catalog.entries.reserve(animations.size());
        for (const auto& node : animations)
        {
            Types::AnimationEntry entry;
            entry.name = node.at("n").get<std::string>();
            entry.sources = node.at("z").get<std::vector<std::uint16_t>>();
            if (std::ranges::any_of(entry.sources, [&](std::uint16_t source) { return source >= catalog.sources.size(); }))
                throw std::runtime_error(entry.name + " is from a zone the catalog doesn't have");
            entry.frames = node.at("f").get<std::uint32_t>();
            entry.rate = node.at("r").get<float>();
            entry.loop = node.value("l", 0) != 0;
            entry.delta = node.value("d", 0) != 0;
            entry.kind = ParseKind(node.at("k").get<std::string>());
            entry.family = ParseFamily(node.at("y").get<std::string>());
            entry.boneSet = node.at("b").get<std::uint32_t>();
            if (entry.boneSet >= catalog.boneSets.size())
                throw std::runtime_error(entry.name + " has a bone set the catalog doesn't have");
            entry.bytes = node.value("s", 0u);
            entry.hash = node.value("h", "");
            if (const auto it = node.find("t"); it != node.end())
                for (const auto& note : *it)
                    entry.notetracks.push_back({notetracks.at(note.at(0).get<std::size_t>()), note.at(1).get<float>()});
            std::ranges::stable_sort(entry.notetracks, {}, &Types::ActorNotetrack::time);
            if (const auto it = node.find("m"); it != node.end() && it->size() == 3)
                entry.motion = glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
            entry.turn = node.value("a", 0.0f);
            if (const auto it = node.find("ff"); it != node.end())
                for (const auto& foot : *it)
                    entry.footfalls.push_back({foot.at(0).get<float>(), foot.at(1).get<int>() != 0});
            if (const auto it = node.find("st"); it != node.end() && it->size() == 4)
            {
                entry.from = {ParsePosture((*it)[0]), ParseMovement((*it)[1])};
                entry.to = {ParsePosture((*it)[2]), ParseMovement((*it)[3])};
            }
            const std::string asset(entry.AssetName());
            entry.death = ActorLogic::IsDeathAnimation(asset, entry.loop, entry.notetracks);
            entry.alert = ActorLogic::IsAlertAnimation(
                asset, ActorLogic::AnimationInfo{entry.Duration(), std::nullopt, entry.notetracks, entry.kind,
                                                 entry.family});
            catalog.entries.push_back(std::move(entry));
        }
        std::ranges::sort(catalog.entries, {}, &Types::AnimationEntry::name);
        return catalog;
    }

    bool IsListed(std::string_view name, Types::AnimationFamily family)
    {
        return !name.starts_with("viewmodel_") && family != Family::Viewmodel;
    }

    ModelRig GetModelRig(std::span<const std::string> bones)
    {
        ModelRig rig;
        rig.bones.insert(bones.begin(), bones.end());
        const auto has = [&](const char* bone) { return rig.bones.contains(bone); };
        rig.dog = std::ranges::any_of(
            bones, [](const std::string& bone) { return PoseLogic::StartsWithAny(bone, PoseLogic::DOG_BONES); });
        rig.human = !rig.dog && has("j_spinelower");
        // (a dog has a jaw and lips too)
        rig.face = !rig.dog && (has("j_jaw") || has("j_lip_top_le") || has("j_brow_le"));
        // the spine rig the game gets for a body of the SP rig (see t5/src/Actors.cpp GetSpineRig)
        if (rig.human && has("j_mainroot") && has("j_spinelower") && !has("torso_stabilizer"))
        {
            rig.bones.insert("pelvis");
            rig.bones.insert("torso_stabilizer");
        }
        return rig;
    }

    bool Fits(const Types::AnimationCatalog& catalog, const Types::AnimationEntry& entry, const ModelRig& rig)
    {
        if (rig.vehicle)
            return std::ranges::find(rig.own, entry.AssetName()) != rig.own.end();
        return FitsBones(catalog, entry.boneSet, GetFitClass(entry), rig);
    }

    std::string DescribeMisfit(const Types::AnimationCatalog& catalog, const Types::AnimationEntry& entry,
                               const ModelRig& rig)
    {
        if (Fits(catalog, entry, rig))
            return {};
        if (!HasBones(catalog, entry.boneSet))
            return "It moves no bones";
        const auto fitClass = GetFitClass(entry);
        if (rig.vehicle)
            switch (fitClass)
            {
                case FitClass::Face:
                    return "Made for a person's face: the actor is a vehicle";
                case FitClass::Human:
                    return "Made for a person's skeleton: the actor is a vehicle";
                case FitClass::Dog:
                    return "Made for a dog's skeleton: the actor is a vehicle";
                case FitClass::Other:
                    return "Made for a prop or vehicle of its own: the game plays none but its own on this vehicle";
            }
        const auto bones = CountBones(catalog, entry.boneSet, fitClass, rig);
        const auto some = std::format("{} of the {} bones it moves", bones.present, bones.total);
        switch (fitClass)
        {
            case FitClass::Face:
                return rig.dog     ? "Made for a person's face: the actor is a dog"
                       : rig.human ? "Made for a face: the actor's models have none (a body without a head)"
                                   : "Made for a face: the actor's models have none";
            case FitClass::Human:
                if (rig.dog)
                    return "Made for a person's skeleton: the actor is a dog";
                if (!rig.human)
                    return "Made for a person's body: the actor's models have none (a head alone, a prop)";
                return std::format("Made for a body with bones the actor's models don't have: they have {}", some);
            case FitClass::Dog:
                if (!rig.dog)
                    return rig.human ? "Made for a dog's skeleton: the actor is a person"
                                     : "Made for a dog's skeleton: the actor's models aren't a dog";
                return std::format("Made for another animal's skeleton: the actor has {}", some);
            case FitClass::Other:
                break;
        }
        return std::format("Made for a prop or vehicle of its own: the actor's models have {}", some);
    }

    std::vector<bool> GetFits(const Types::AnimationCatalog& catalog, const ModelRig& rig)
    {
        // by bone set and class: -1 not worked out yet
        std::vector<std::array<std::int8_t, 4>> known(catalog.boneSets.size(), {-1, -1, -1, -1});
        std::vector<bool> fits(catalog.entries.size(), false);
        if (rig.vehicle)
        {
            for (std::size_t i = 0; i < catalog.entries.size(); ++i)
                fits[i] = Fits(catalog, catalog.entries[i], rig);
            return fits;
        }
        for (std::size_t i = 0; i < catalog.entries.size(); ++i)
        {
            const auto& entry = catalog.entries[i];
            if (entry.boneSet >= known.size())
                continue;
            const auto fitClass = GetFitClass(entry);
            auto& slot = known[entry.boneSet][static_cast<std::size_t>(fitClass)];
            if (slot < 0)
                slot = FitsBones(catalog, entry.boneSet, fitClass, rig) ? 1 : 0;
            fits[i] = slot == 1;
        }
        return fits;
    }

    bool Matches(const Types::AnimationCatalog& catalog, const Types::AnimationEntry& entry, std::string_view search)
    {
        const auto name = Lower(entry.name);
        std::string where;
        for (const auto source : entry.sources)
            if (source < catalog.sources.size())
                where += Lower(catalog.sources[source].zone) + " " + Lower(catalog.sources[source].name) + " ";
        std::size_t start = 0;
        while (start < search.size())
        {
            auto end = search.find(' ', start);
            if (end == std::string_view::npos)
                end = search.size();
            if (end > start)
            {
                const auto word = Lower(search.substr(start, end - start));
                if (name.find(word) == std::string::npos && where.find(word) == std::string::npos)
                    return false;
            }
            start = end + 1;
        }
        return true;
    }

    std::vector<const Types::AnimationEntry*> Search(const Types::AnimationCatalog& catalog, std::string_view search)
    {
        std::vector<const Types::AnimationEntry*> found;
        for (const auto& entry : catalog.entries)
            if (Matches(catalog, entry, search))
                found.push_back(&entry);
        return found;
    }

    std::string_view GetCategory(std::string_view name, Types::AnimationKind kind, const Types::AnimationEntry* entry)
    {
        if (!entry && Components::KillstreakLogic::IsDeathMotion(name))
            return "Deaths";
        if (!entry && Components::KillstreakLogic::IsMotion(name))
            return "Movement";
        if ((entry ? entry->kind : kind) == Kind::Face)
            return "Faces";
        auto has = [&](std::initializer_list<std::string_view> parts) {
            return std::ranges::any_of(
                parts, [&](std::string_view part) { return name.find(part) != std::string_view::npos; });
        };
        if (entry ? entry->death : has({"death", "dying", "die_", "_dead"}))
            return "Deaths";
        if (entry && entry->family == Family::Dog)
            return "Animals";
        if (entry && entry->family == Family::Other && entry->kind == Kind::Other)
            return "Props and vehicles";
        if (has({"pain", "flinch", "react", "stumble", "flash", "shellshock", "stun"}))
            return "Pain and reactions";
        if (has({"melee", "knife", "stab", "punch"}))
            return "Melee";
        if (has({"cover", "corner", "pillar", "lean"}))
            return "Cover";
        // moving, changing stance or turning
        if (entry && (IsMoving(entry->from.movement) || IsMoving(entry->to.movement) ||
                      (entry->from.posture != entry->to.posture && entry->from.posture != Posture::Unknown &&
                       entry->to.posture != Posture::Unknown)))
            return "Movement";
        if (has({"walk", "run", "sprint", "jog", "strafe", "turn", "step", "shuffle", "arrive", "exit", "ladder",
                 "jump", "climb", "traverse", "crawl", "move", "_to_"}))
            return "Movement";
        if (has({"shoot", "fire", "aim", "reload", "grenade", "throw", "cqb", "exposed", "ads", "burst", "semi"}))
            return "Combat";
        if (has({"idle", "stand", "casual", "twitch", "wait", "look", "crouch", "prone", "sit"}))
            return "Idle and poses";
        return "Other";
    }

    const std::vector<std::string_view>& GetCategories()
    {
        static const std::vector<std::string_view> categories{
            "Idle and poses", "Movement", "Combat", "Cover",   "Melee",
            "Pain and reactions", "Deaths", "Faces", "Animals", "Props and vehicles", "Other"};
        return categories;
    }

    std::string GetHeading(const Types::AnimationSource& source)
    {
        return source.name == source.group || source.name.empty() ? source.group
                                                                  : std::format("{} ({})", source.name, source.group);
    }

    AnimationList List(const Types::AnimationCatalog& catalog, const Types::ActorCatalog& game)
    {
        AnimationList list;
        const auto& categories = GetCategories();
        const auto categoryOf = [&](std::string_view name, Kind kind, const Types::AnimationEntry* entry) {
            const auto category = GetCategory(name, kind, entry);
            return static_cast<std::size_t>(std::ranges::find(categories, category) - categories.begin());
        };
        // a heading for each source the catalog lists, in its order (a map's patch zone shares the map's)
        std::vector<std::size_t> headingOf(catalog.sources.size());
        for (std::size_t i = 0; i < catalog.sources.size(); ++i)
        {
            const auto heading = GetHeading(catalog.sources[i]);
            const auto it = std::ranges::find(list.headings, heading);
            headingOf[i] = static_cast<std::size_t>(it - list.headings.begin());
            if (it == list.headings.end())
                list.headings.push_back(heading);
        }
        // the killstreaks' motions, first (with the fit filter on, a killstreak's list is them and its own animations)
        list.animations.reserve(catalog.entries.size() + 16);
        std::size_t motionHeading = SIZE_MAX;
        for (const auto& animation : game.animations)
        {
            if (!Components::KillstreakLogic::IsMotion(animation.name))
                continue;
            if (motionHeading == SIZE_MAX)
            {
                motionHeading = list.headings.size();
                list.headings.emplace_back("Vehicle motions");
            }
            Listed listed{.name = animation.name, .loaded = &animation, .kind = animation.kind};
            listed.category = categoryOf(animation.name, animation.kind, nullptr);
            listed.heading = motionHeading;
            listed.lowerName = Lower(animation.name) + " " +
                               Lower(Components::KillstreakLogic::GetMotionLabel(animation.name));
            listed.lowerWhere = "vehicle motions killstreak";
            list.animations.push_back(std::move(listed));
        }
        std::vector<std::size_t> order(catalog.entries.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        const auto headingOfEntry = [&](const Types::AnimationEntry& entry) {
            return entry.sources.empty() ? std::size_t{0} : headingOf[entry.sources.front()];
        };
        // (the entries are sorted by name already)
        std::ranges::stable_sort(order, {}, [&](std::size_t i) { return headingOfEntry(catalog.entries[i]); });
        for (const auto i : order)
        {
            const auto& entry = catalog.entries[i];
            Listed listed{.name = entry.name, .entry = &entry, .index = i, .kind = entry.kind};
            listed.loaded = game.FindAnimation(entry.name);
            listed.category = categoryOf(entry.name, entry.kind, &entry);
            listed.heading = headingOfEntry(entry);
            listed.lowerName = Lower(entry.name);
            for (const auto source : entry.sources)
                listed.lowerWhere += Lower(catalog.sources[source].zone) + " " + Lower(catalog.sources[source].name) + " ";
            list.animations.push_back(std::move(listed));
        }
        // what the game has loaded that the catalog doesn't (of a game newer than the catalog, say)
        std::size_t loadedHeading = SIZE_MAX;
        for (const auto& animation : game.animations)
        {
            if (catalog.Find(animation.name) || !IsListed(animation.name, animation.family) ||
                Components::KillstreakLogic::IsMotion(animation.name))
                continue;
            if (loadedHeading == SIZE_MAX)
            {
                loadedHeading = list.headings.size();
                list.headings.emplace_back("Loaded in the game");
            }
            Listed listed{.name = animation.name, .loaded = &animation, .kind = animation.kind};
            listed.category = categoryOf(animation.name, animation.kind, nullptr);
            listed.heading = loadedHeading;
            listed.lowerName = Lower(animation.name);
            listed.lowerWhere = Lower(list.headings[loadedHeading]);
            list.animations.push_back(std::move(listed));
        }
        return list;
    }

    std::optional<Types::ActorStance> GetStance(Types::AnimationPosture posture)
    {
        switch (posture)
        {
            case Posture::Stand:
                return Types::ActorStance::Stand;
            case Posture::Crouch:
                return Types::ActorStance::Crouch;
            case Posture::Prone:
            case Posture::Back:
                return Types::ActorStance::Prone;
            default:
                return std::nullopt;
        }
    }

    std::string_view GetPostureName(Types::AnimationPosture posture)
    {
        constexpr std::array<std::string_view, 5> NAMES{"-", "stand", "crouch", "prone", "back"};
        return NAMES[static_cast<std::size_t>(posture)];
    }

    std::string_view GetMovementName(Types::AnimationMovement movement)
    {
        constexpr std::array<std::string_view, 5> NAMES{"-", "stop", "walk", "run", "sprint"};
        return NAMES[static_cast<std::size_t>(movement)];
    }

    std::string DescribeState(const Types::AnimationState& state)
    {
        if (state == Types::AnimationState{})
            return "-";
        return std::format("{} {}", GetPostureName(state.posture), GetMovementName(state.movement));
    }
}  // namespace IWXMVM::AnimationCatalogLogic
