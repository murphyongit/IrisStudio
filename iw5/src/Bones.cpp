#include "StdInclude.hpp"
#include "Bones.hpp"

#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "Events.hpp"
#include "Functions.hpp"
#include "Structures.hpp"

namespace IWXMVM::IW5::Bones
{
    namespace
    {
        constexpr std::int32_t MAX_ENTITIES = 0x800;
        // bones keep being looked up for this many frames after they were last asked for
        constexpr int KEEP_UPDATES = 60;

        using BoneKey = std::pair<std::int32_t, std::string>;

        // main thread only. The strings belong to the game's script strings, which a map's load rebuilds.
        std::unordered_map<std::string, std::uint16_t> scriptStrings;

        std::mutex mutex;
        std::set<BoneKey> requests;
        std::map<BoneKey, Types::BoneData> results;
        // main thread only: frames since each bone was last asked for
        std::map<BoneKey, int> watched;

        std::uint16_t GetScriptString(const std::string& name)
        {
            auto it = scriptStrings.find(name);
            if (it == scriptStrings.end())
                it = scriptStrings
                         .emplace(name, static_cast<std::uint16_t>(Functions::Scr_AllocString(name.c_str())))
                         .first;
            return it->second;
        }

        int FindBoneIndex(const Structures::DObj* dobj, std::uint16_t boneName)
        {
            if (!dobj->models || !dobj->numModels)
                return -1;

            int total = 0;
            for (int m = 0; m < dobj->numModels; m++)
            {
                const auto model = dobj->models[m];
                if (!model || !model->numBones || !model->boneNames)
                    return -1;
                for (int b = 0; b < model->numBones; b++)
                {
                    if (model->boneNames[b] == boneName)
                        return total + b;
                }
                total += model->numBones;
            }
            return -1;
        }

        Types::BoneData LookUpBone(std::int32_t entityId, const std::string& name)
        {
            if (entityId < 0 || entityId >= MAX_ENTITIES)
                return {.id = -1};

            const auto dobj = Functions::Com_GetClientDObj(entityId);
            const auto entities = Structures::GetEntities();
            if (!dobj || !entities)
                return {.id = -1};

            const auto boneIndex = FindBoneIndex(dobj, GetScriptString(name));
            if (boneIndex < 0 || boneIndex >= dobj->numBones)
                return {.id = -1};

            // (the game makes the model's skeleton for this frame itself if nothing has asked for it yet)
            float axis[3 * 3];
            float origin[3];
            if (!Functions::CG_DObjGetWorldBoneMatrix(&entities[entityId], boneIndex, axis, dobj, origin))
                return {.id = -1};

            Types::BoneData bone;
            bone.id = boneIndex;
            bone.position = glm::make_vec3(origin);
            bone.rotation = glm::make_mat3(axis);
            return bone;
        }
    }  // namespace

    Types::BoneData GetBoneData(std::int32_t entityId, const std::string& name)
    {
        std::lock_guard lock(mutex);
        BoneKey key{entityId, name};
        requests.insert(key);

        const auto result = results.find(key);
        return result != results.end() ? result->second : Types::BoneData{.id = -1};
    }

    void UpdateRequestedBones()
    {
        std::set<BoneKey> pending;
        {
            std::lock_guard lock(mutex);
            pending.swap(requests);
        }

        for (auto it = watched.begin(); it != watched.end();)
            it = ++it->second > KEEP_UPDATES ? watched.erase(it) : std::next(it);
        for (const auto& key : pending)
            watched[key] = 0;

        std::map<BoneKey, Types::BoneData> found;
        for (const auto& [key, age] : watched)
            found[key] = LookUpBone(key.first, key.second);

        std::lock_guard lock(mutex);
        results.swap(found);
    }

    void Install()
    {
        // (both on the main thread: the demo command raises them)
        const auto forget = [] {
            scriptStrings.clear();
            watched.clear();
            std::lock_guard lock(mutex);
            requests.clear();
            results.clear();
        };
        Events::RegisterListener(EventType::PreDemoLoad, forget);
        Events::RegisterListener(EventType::PostDemoLoad, forget);
    }
}  // namespace IWXMVM::IW5::Bones
