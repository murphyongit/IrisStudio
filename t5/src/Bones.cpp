#include "StdInclude.hpp"
#include "Bones.hpp"

#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "Events.hpp"
#include "Mod.hpp"
#include "Actors.hpp"
#include "AnimationPose.hpp"
#include "Functions.hpp"
#include "Structures.hpp"
#include "Hooks/Camera.hpp"

namespace IWXMVM::T5::Bones
{
    using BoneKey = std::pair<std::int32_t, std::string>;
    // a target and a bone; no bone for the target's list of bones
    using TargetKey = std::tuple<Types::BoneCameraTargetKind, std::int32_t, std::string>;

    // main thread only. The strings belong to the script string table, which is rebuilt when a map loads.
    std::unordered_map<std::string, std::uint16_t> scriptStrings;

    std::uint16_t GetScriptString(const std::string& name)
    {
        auto it = scriptStrings.find(name);
        if (it == scriptStrings.end())
            it = scriptStrings.emplace(name, Functions::SL_GetString(name.c_str())).first;

        return it->second;
    }

    Types::BoneData LookUpBone(std::int32_t entityId, const std::string& name)
    {
        if (entityId < 0 || entityId >= static_cast<std::int32_t>(Structures::MAX_GENTITIES))
            return {.id = -1};

        const auto entities = Structures::GetEntities();
        const auto obj = Functions::Com_GetClientDObj(entityId, 0);
        if (!entities || !obj)
            return {.id = -1};

        const auto boneName = GetScriptString(name);
        const auto boneIndex = Functions::DObjGetBoneIndex(obj, boneName);
        if (!boneIndex)
            return {.id = -1};

        // From the pose the model is drawn with, which for the player a demo was recorded from isn't the one in
        // cg_entities (see CG_GetPose). The game works out a model's skeleton once per frame, from whichever pose asks
        // first, so looking bones up from that stale pose also made the model twitch into it on some frames.
        float axis[3][3];
        float origin[3];
        if (!Functions::CG_DObjGetWorldTagMatrix(Functions::CG_GetPose(0, entityId), obj, boneName, axis, origin))
            return {.id = -1};

        return {
            .id = *boneIndex,
            .position = glm::make_vec3(origin),
            .rotation = glm::make_mat3(&axis[0][0]),
        };
    }

    std::int32_t ResolvePlayer(std::int32_t entityId)
    {
        const auto entities = Structures::GetEntities();
        if (!entities || entityId < 0 || entityId >= static_cast<std::int32_t>(Structures::MAX_CLIENTS))
            return entityId;
        const auto& player = entities[entityId];
        if ((player.flags & Structures::CENTITY_IN_SNAPSHOT) || player.nextState.eType != Structures::ET_PLAYER)
            return entityId;
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(Structures::MAX_GENTITIES); ++i)
        {
            if (entities[i].nextState.eType == Structures::ET_PLAYER_CORPSE &&
                entities[i].nextState.clientNum == entityId)
                return i;
        }
        return entityId;
    }

    std::optional<Types::TargetBone> LookUpTargetBone(const Types::BoneCameraTarget& target, const std::string& name)
    {
        if (target.kind == Types::BoneCameraTargetKind::Actor)
            return Actors::GetDrawnBone(static_cast<std::uint32_t>(target.id), name);

        const auto entity = ResolvePlayer(target.id);
        const auto bone = LookUpBone(entity, name);
        const auto pose = Functions::CG_GetPose(0, entity);
        if (bone.id == -1 || !pose)
            return std::nullopt;
        return Types::TargetBone{bone.position, bone.rotation, glm::make_vec3(pose->origin), pose->angles[1]};
    }

    std::vector<std::string> LookUpTargetBones(const Types::BoneCameraTarget& target)
    {
        if (target.kind == Types::BoneCameraTargetKind::Actor)
            return Actors::GetBuiltBones(static_cast<std::uint32_t>(target.id));

        const auto entity = ResolvePlayer(target.id);
        if (entity < 0 || entity >= static_cast<std::int32_t>(Structures::MAX_GENTITIES))
            return {};
        const auto obj = Functions::Com_GetClientDObj(entity, 0);
        std::vector<std::string> bones;
        for (std::uint32_t i = 0; obj && i < obj->numModels; ++i)
        {
            for (auto& name : AnimationPose::GetBoneNames(*obj->models[i]))
            {
                if (std::ranges::find(bones, name) == bones.end())
                    bones.push_back(std::move(name));
            }
        }
        return bones;
    }

    std::mutex mutex;
    std::set<BoneKey> requests;
    std::map<BoneKey, Types::BoneData> results;
    std::set<TargetKey> targetRequests;
    std::map<TargetKey, std::optional<Types::TargetBone>> targetResults;
    std::map<TargetKey, std::vector<std::string>> boneListResults;

    // Bones keep being looked up for a while after they were last asked for, so callers that don't ask every frame
    // still find them. Main thread only; the value is the number of updates since the last request.
    constexpr int KEEP_UPDATES = 60;
    std::map<BoneKey, int> watchedBones;
    std::map<TargetKey, int> watchedTargets;

    // From the moment a demo starts to load until the world's next frame. The game still says it's in a demo then, as
    // cl_ingame is only set by its frames, while the client's memory for skeletons is gone: asking for a bone ended the
    // game (writing through it at 0x621037), if the bone camera or a light on a bone had asked in the last frames.
    std::atomic<bool> loading = false;

    Types::BoneData GetBoneData(std::int32_t entityId, const std::string& name)
    {
        if (Hooks::Camera::IsMainThread())
            return LookUpBone(entityId, name);

        std::lock_guard lock(mutex);
        BoneKey key{entityId, name};
        requests.insert(key);

        const auto result = results.find(key);
        return result != results.end() ? result->second : Types::BoneData{.id = -1};
    }

    std::optional<Types::TargetBone> GetTargetBone(const Types::BoneCameraTarget& target, const std::string& name)
    {
        if (Hooks::Camera::IsMainThread())
            return LookUpTargetBone(target, name);

        std::lock_guard lock(mutex);
        TargetKey key{target.kind, target.id, name};
        targetRequests.insert(key);

        const auto result = targetResults.find(key);
        return result != targetResults.end() ? result->second : std::nullopt;
    }

    std::vector<std::string> GetTargetBones(const Types::BoneCameraTarget& target)
    {
        if (Hooks::Camera::IsMainThread())
            return LookUpTargetBones(target);

        std::lock_guard lock(mutex);
        TargetKey key{target.kind, target.id, ""};
        targetRequests.insert(key);

        const auto result = boneListResults.find(key);
        return result != boneListResults.end() ? result->second : std::vector<std::string>{};
    }

    void UpdateRequestedBones()
    {
        std::set<BoneKey> pending;
        std::set<TargetKey> pendingTargets;
        {
            std::lock_guard lock(mutex);
            pending.swap(requests);
            pendingTargets.swap(targetRequests);
        }

        for (auto it = watchedBones.begin(); it != watchedBones.end();)
            it = ++it->second > KEEP_UPDATES ? watchedBones.erase(it) : std::next(it);
        for (const auto& key : pending)
            watchedBones[key] = 0;
        for (auto it = watchedTargets.begin(); it != watchedTargets.end();)
            it = ++it->second > KEEP_UPDATES ? watchedTargets.erase(it) : std::next(it);
        for (const auto& key : pendingTargets)
            watchedTargets[key] = 0;

        const bool inDemo = !loading && Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        std::map<BoneKey, Types::BoneData> found;
        std::map<TargetKey, std::optional<Types::TargetBone>> foundTargets;
        std::map<TargetKey, std::vector<std::string>> foundLists;
        for (const auto& [key, age] : watchedBones)
        {
            if (inDemo)
                found[key] = LookUpBone(key.first, key.second);
        }
        // (the players' bones now; the actors' once they are drawn, see UpdateRequestedActorBones)
        for (const auto& [key, age] : watchedTargets)
        {
            const auto& [kind, id, name] = key;
            const Types::BoneCameraTarget target{kind, id};
            if (name.empty())
                foundLists[key] = inDemo ? LookUpTargetBones(target) : std::vector<std::string>{};
            else if (kind == Types::BoneCameraTargetKind::Player && inDemo)
                foundTargets[key] = LookUpTargetBone(target, name);
        }

        std::lock_guard lock(mutex);
        results.swap(found);
        boneListResults.swap(foundLists);
        // what the actors had stays until they are drawn again
        for (auto it = targetResults.begin(); it != targetResults.end();)
        {
            const bool keep = std::get<0>(it->first) == Types::BoneCameraTargetKind::Actor &&
                              watchedTargets.contains(it->first) && inDemo;
            it = keep ? std::next(it) : targetResults.erase(it);
        }
        targetResults.merge(foundTargets);
    }

    void UpdateRequestedActorBones()
    {
        // (a frame of the world: see loading)
        loading = false;

        std::map<TargetKey, std::optional<Types::TargetBone>> found;
        for (const auto& [key, age] : watchedTargets)
        {
            const auto& [kind, id, name] = key;
            if (kind == Types::BoneCameraTargetKind::Actor && !name.empty())
                found[key] = Actors::GetDrawnBone(static_cast<std::uint32_t>(id), name);
        }
        if (found.empty())
            return;

        std::lock_guard lock(mutex);
        for (auto& [key, bone] : found)
            targetResults[key] = bone;
    }

    void Install()
    {
        // raised on the main thread by the demo command, once the demo's map has loaded
        Events::RegisterListener(EventType::PostDemoLoad, [] { scriptStrings.clear(); });
        Events::RegisterListener(EventType::PreDemoLoad, [] { loading = true; });
    }
}  // namespace IWXMVM::T5::Bones
