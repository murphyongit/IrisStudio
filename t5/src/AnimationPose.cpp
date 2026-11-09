#include "StdInclude.hpp"
#include "AnimationPose.hpp"
#include "Components/GripLogic.hpp"

#include <glm/gtc/quaternion.hpp>

#include "AnimationStore.hpp"
#include "ContentPacks.hpp"
#include "Functions.hpp"

namespace IWXMVM::T5::AnimationPose
{
    using namespace Structures;

    // An animation's data is laid out like IW3's, worked out by decoding pb_stand_death_head_collapse and comparing
    // with the bones the game worked out for a corpse playing it: all 85 bones of the body matched to 0.01 units.
    // Each stream is read in order:
    // - for each bone with keys, its number of keys minus one (dataShort), the frames of the keys and the values
    //   (randomDataShort for rotations and precise translations, randomDataByte for small translations). The frames
    //   are bytes in dataByte for fewer than 256 frames; from 256 on they are shorts, in dataShort for fewer than 65
    //   keys, else in indices, after a table in dataShort ((keys - 2) / 256 + 2 shorts) that only speeds up the
    //   game's search (KisakCOD's XAnimCalcParts, src/xanim/xanim_calc.cpp);
    // - rotations as quaternions (x, y, z, w) of shorts over 32767, the half ones only z and w;
    // - for translations the bone (dataByte, into names), then the smallest value and the size of a step (dataInt);
    //   a translation is added to the bone's rest position.
    // The frame at a time from 0 to 1 is time * numframes.

    struct Track
    {
        std::vector<std::uint16_t> frames;
        std::vector<glm::vec4> values;  // quaternions as x, y, z, w; translations in x, y, z
    };

    struct DecodedAnimation
    {
        std::uint16_t numFrames;
        float duration;
        std::vector<std::optional<Track>> rotations;     // by bone of the body
        std::vector<std::optional<Track>> translations;  // by bone of the body
        std::vector<std::string> boneNames;              // the body's
        // for an animation made for SP bodies on an MP body, the spine to line up (see PoseLogic::SpineKind)
        std::optional<SpineRig> linedSpine;
        // for one made for MP bodies on an SP body, the spine rig it is drawn with (see FitSpineChain), the body's
        // j_spinelower and the tracks of the rig's bones the body hasn't: pelvis and the stabilizer
        std::optional<PoseLogic::SpineChain> spineChain;
        std::uint32_t spineBone = 0;
        std::array<std::optional<Track>, 2> rigRotations;
        std::array<std::optional<Track>, 2> rigTranslations;
    };

    template <typename T>
    struct Stream
    {
        const T* data;
        std::size_t count;
        std::size_t position = 0;

        bool Take(std::size_t n, const T*& out)
        {
            if (!data || position + n > count)
                return false;
            out = data + position;
            position += n;
            return true;
        }
    };

    std::vector<std::string> GetBoneNames(const XModel& model);

    // the rig's bones an SP body hasn't, which the decoder keeps the tracks of
    inline constexpr std::array<std::string_view, 2> RIG_BONES = {"pelvis", "torso_stabilizer"};

    std::optional<DecodedAnimation> Decode(const XAnimParts& parts, const XModel& body)
    {
        if (parts.numframes == 0 || parts.framerate <= 0.0f)
            return std::nullopt;

        Stream<std::uint8_t> dataByte{parts.dataByte, parts.dataByteCount};
        Stream<std::int16_t> dataShort{parts.dataShort, parts.dataShortCount};
        Stream<float> dataInt{parts.dataInt, parts.dataIntCount};
        Stream<std::int16_t> randomDataShort{parts.randomDataShort, parts.randomDataShortCount};
        Stream<std::uint8_t> randomDataByte{parts.randomDataByte, parts.randomDataByteCount};
        const bool longFrames = parts.numframes >= 256;
        Stream<std::uint16_t> indices{longFrames ? static_cast<const std::uint16_t*>(parts.indices) : nullptr,
                                      longFrames ? parts.indexCount : 0};

        DecodedAnimation animation{
            .numFrames = parts.numframes,
            .duration = parts.numframes / parts.framerate,
            .rotations = std::vector<std::optional<Track>>(body.numBones),
            .translations = std::vector<std::optional<Track>>(body.numBones),
        };

        // the body's bone for each of the animation's
        const auto boneCount = parts.boneCount[PART_TYPE_ALL];
        std::vector<std::int32_t> bodyBones(boneCount, -1);
        for (std::uint32_t i = 0; i < boneCount; ++i)
        {
            for (std::uint32_t b = 0; b < body.numBones; ++b)
            {
                if (body.boneNames[b] == parts.names[i])
                    bodyBones[i] = static_cast<std::int32_t>(b);
            }
        }
        animation.boneNames = GetBoneNames(body);
        if (GetSpineKind(parts) == PoseLogic::SpineKind::Lined)
            animation.linedSpine = FindSpineRig(body);
        std::vector<std::int32_t> rigBones(boneCount, -1);  // the index into RIG_BONES of each of the animation's
        if (ClassifyAnimation(parts).family == Types::AnimationFamily::MpPlayer)
            animation.spineChain = FitSpineChain(body);
        if (animation.spineChain)
        {
            animation.spineBone = static_cast<std::uint32_t>(FindBone(body, "j_spinelower"));
            for (std::uint32_t i = 0; i < boneCount; ++i)
            {
                const auto rigBone = std::ranges::find(RIG_BONES, Functions::SL_ConvertToString(parts.names[i]));
                if (bodyBones[i] < 0 && rigBone != RIG_BONES.end())
                    rigBones[i] = static_cast<std::int32_t>(rigBone - RIG_BONES.begin());
            }
        }

        const auto readKeys = [&](Track& track) {
            const std::int16_t* keyCount;
            if (!dataShort.Take(1, keyCount))
                return false;
            const std::size_t last = static_cast<std::uint16_t>(*keyCount);
            if (!longFrames)
            {
                const std::uint8_t* frames;
                if (!dataByte.Take(last + 1, frames))
                    return false;
                track.frames.assign(frames, frames + last + 1);
                return true;
            }
            const std::uint16_t* frames;
            if (last >= 64)
            {
                const std::int16_t* table;
                if (!dataShort.Take(((last - 1) >> 8) + 2, table) || !indices.Take(last + 1, frames))
                    return false;
            }
            else
            {
                const std::int16_t* shorts;
                if (!dataShort.Take(last + 1, shorts))
                    return false;
                frames = reinterpret_cast<const std::uint16_t*>(shorts);
            }
            track.frames.assign(frames, frames + last + 1);
            return true;
        };

        const auto store = [&](bool rotation, std::uint32_t animBone, Track&& track) {
            if (animBone >= bodyBones.size())
                return;
            if (bodyBones[animBone] >= 0)
                (rotation ? animation.rotations : animation.translations)[bodyBones[animBone]] = std::move(track);
            else if (rigBones[animBone] >= 0)
                (rotation ? animation.rigRotations : animation.rigTranslations)[rigBones[animBone]] = std::move(track);
        };

        std::uint32_t bone = parts.boneCount[PART_TYPE_NO_QUAT];
        for (const auto type : {PART_TYPE_HALF_QUAT, PART_TYPE_FULL_QUAT})
        {
            const std::size_t components = type == PART_TYPE_HALF_QUAT ? 2 : 4;
            for (std::uint32_t i = 0; i < parts.boneCount[type]; ++i, ++bone)
            {
                Track track;
                if (!readKeys(track))
                    return std::nullopt;
                for (std::size_t k = 0; k < track.frames.size(); ++k)
                {
                    const std::int16_t* q;
                    if (!randomDataShort.Take(components, q))
                        return std::nullopt;
                    track.values.push_back(components == 2 ? glm::vec4(0, 0, q[0], q[1]) / 32767.0f
                                                           : glm::vec4(q[0], q[1], q[2], q[3]) / 32767.0f);
                }
                store(true, bone, std::move(track));
            }
        }
        for (const auto type : {PART_TYPE_HALF_QUAT_NO_SIZE, PART_TYPE_FULL_QUAT_NO_SIZE})
        {
            const std::size_t components = type == PART_TYPE_HALF_QUAT_NO_SIZE ? 2 : 4;
            for (std::uint32_t i = 0; i < parts.boneCount[type]; ++i, ++bone)
            {
                const std::int16_t* q;
                if (!dataShort.Take(components, q))
                    return std::nullopt;
                Track track{.frames = {0}};
                track.values.push_back(components == 2 ? glm::vec4(0, 0, q[0], q[1]) / 32767.0f
                                                       : glm::vec4(q[0], q[1], q[2], q[3]) / 32767.0f);
                store(true, bone, std::move(track));
            }
        }

        for (const auto type : {PART_TYPE_SMALL_TRANS, PART_TYPE_TRANS})
        {
            for (std::uint32_t i = 0; i < parts.boneCount[type]; ++i)
            {
                const std::uint8_t* animBone;
                Track track;
                const float* minsAndSize;
                if (!dataByte.Take(1, animBone) || !readKeys(track) || !dataInt.Take(6, minsAndSize))
                    return std::nullopt;

                const glm::vec3 mins(minsAndSize[0], minsAndSize[1], minsAndSize[2]);
                const glm::vec3 size(minsAndSize[3], minsAndSize[4], minsAndSize[5]);
                for (std::size_t k = 0; k < track.frames.size(); ++k)
                {
                    glm::vec3 steps;
                    if (type == PART_TYPE_SMALL_TRANS)
                    {
                        const std::uint8_t* v;
                        if (!randomDataByte.Take(3, v))
                            return std::nullopt;
                        steps = {v[0], v[1], v[2]};
                    }
                    else
                    {
                        const std::int16_t* v;
                        if (!randomDataShort.Take(3, v))
                            return std::nullopt;
                        steps = {static_cast<std::uint16_t>(v[0]), static_cast<std::uint16_t>(v[1]),
                                 static_cast<std::uint16_t>(v[2])};
                    }
                    track.values.emplace_back(mins + size * steps, 0.0f);
                }
                store(false, *animBone, std::move(track));
            }
        }
        for (std::uint32_t i = 0; i < parts.boneCount[PART_TYPE_TRANS_NO_SIZE]; ++i)
        {
            const std::uint8_t* animBone;
            const float* t;
            if (!dataByte.Take(1, animBone) || !dataInt.Take(3, t))
                return std::nullopt;
            store(false, *animBone, Track{.frames = {0}, .values = {glm::vec4(t[0], t[1], t[2], 0)}});
        }

        return animation;
    }

    glm::vec4 Sample(const Track& track, float frame, bool rotation)
    {
        const auto next = std::upper_bound(track.frames.begin(), track.frames.end(), frame);
        if (next == track.frames.begin())
            return track.values.front();
        if (next == track.frames.end())
            return track.values.back();

        const auto i = static_cast<std::size_t>(next - track.frames.begin());
        const float t = (frame - track.frames[i - 1]) / static_cast<float>(track.frames[i] - track.frames[i - 1]);
        auto a = track.values[i - 1];
        auto b = track.values[i];
        if (rotation && glm::dot(a, b) < 0.0f)
            b = -b;
        return a + (b - a) * t;
    }

    glm::quat ToQuat(const glm::vec4& q)
    {
        return glm::normalize(glm::quat(q.w, q.x, q.y, q.z));
    }

    // what the main thread handed over; the animations and the body are assets of the map, loaded until the next one
    std::mutex mutex;
    const XAnim_s* sourceAnims = nullptr;
    const XModel* sourceBody = nullptr;
    std::unordered_map<std::uint32_t, std::optional<DecodedAnimation>> decoded;

    void SetSource(const DObj* obj)
    {
        if (!obj || !obj->tree || !obj->tree->anims || obj->numModels == 0 || !obj->models || !obj->models[0])
            return;

        // (a campaign body on a player has its spine rig in front of it)
        const auto body = obj->numModels > 1 && obj->models[0]->numsurfs == 0 ? obj->models[1] : obj->models[0];
        std::lock_guard lock(mutex);
        if (sourceAnims == obj->tree->anims && sourceBody == body)
            return;

        sourceAnims = obj->tree->anims;
        sourceBody = body;
        decoded.clear();
    }

    void Reset()
    {
        std::lock_guard lock(mutex);
        sourceAnims = nullptr;
        sourceBody = nullptr;
        decoded.clear();
    }

    Types::AnimationPose PoseAt(const DecodedAnimation& anim, const XModel& body, float time);

    std::optional<Types::AnimationPose> GetPose(std::uint32_t animation, float time)
    {
        std::lock_guard lock(mutex);
        if (!sourceAnims || !sourceBody || animation >= sourceAnims->size)
            return std::nullopt;

        auto it = decoded.find(animation);
        if (it == decoded.end())
        {
            const auto& entry = sourceAnims->entries[animation];
            it = decoded.emplace(animation, entry.numAnims == 0 && entry.parts ? Decode(*entry.parts, *sourceBody)
                                                                              : std::nullopt)
                     .first;
        }
        if (!it->second)
            return std::nullopt;
        return PoseAt(*it->second, *sourceBody, time);
    }

    // the decoded animations by name, for the body they were decoded for, while the pack generation they are of lasts
    std::map<std::pair<std::string, std::string>, std::optional<DecodedAnimation>, std::less<>> decodedByName;
    std::uint32_t decodedPackGeneration = 0;

    // the map's MP spine (see SetSpineChain), taken after mutex where both are
    std::mutex chainMutex;
    std::optional<PoseLogic::SpineChain> spineChain;

    void SetSpineChain(std::optional<PoseLogic::SpineChain> chain)
    {
        // what was decoded with another goes
        std::lock_guard lock(mutex);
        std::lock_guard chainLock(chainMutex);
        spineChain = chain;
        decoded.clear();
        decodedByName.clear();
    }

    std::optional<PoseLogic::SpineChain> GetSpineChain()
    {
        std::lock_guard lock(chainMutex);
        return spineChain;
    }

    std::optional<Types::AnimationPose> GetPose(std::string_view animation, std::string_view model, float time)
    {
        // the assets come and go with packs, and the asset table changes while they load
        const auto assets = ContentPacks::TryReadAssets();
        if (!assets.owns_lock() || ContentPacks::IsLoading())
            return std::nullopt;
        std::lock_guard lock(mutex);
        if (const auto generation = ContentPacks::GetPackState().generation; generation != decodedPackGeneration)
        {
            decodedByName.clear();
            decodedPackGeneration = generation;
        }

        const auto body = static_cast<const XModel*>(ContentPacks::FindAsset(ASSET_TYPE_XMODEL, std::string(model).c_str()));
        if (!body)
            return std::nullopt;
        const auto key = std::pair(std::string(animation), std::string(model));
        auto it = decodedByName.find(key);
        if (it == decodedByName.end())
        {
            // the game's, or the animation store's once it has loaded it (decoded with a reference that keeps it
            // loaded meanwhile)
            const auto found = AnimationStore::FindLoaded(key.first);
            if (!found.parts)
                return std::nullopt;
            if (decodedByName.size() > 64)
                decodedByName.clear();
            it = decodedByName.emplace(key, Decode(*found.parts, *body)).first;
        }
        if (!it->second)
            return std::nullopt;
        return PoseAt(*it->second, *body, time);
    }

    // a model's bones at rest, relative to the bones they hang from; the root bones at the model's origin
    std::vector<PoseLogic::LocalBone> GetRestBones(const XModel& model)
    {
        std::vector<PoseLogic::LocalBone> bones(model.numBones);
        for (std::uint32_t b = model.numRootBones; b < model.numBones; ++b)
        {
            const auto i = b - model.numRootBones;
            const auto& q = model.quats[i];
            bones[b] = {.rotation = ToQuat(glm::vec4(q[0], q[1], q[2], q[3])),
                        .translation = glm::make_vec3(model.trans[i]),
                        .parent = static_cast<std::int32_t>(b) - model.parentList[i]};
        }
        return bones;
    }

    std::vector<std::string> GetBoneNames(const XModel& model)
    {
        std::vector<std::string> names;
        for (std::uint32_t b = 0; b < model.numBones; ++b)
            names.emplace_back(Functions::SL_ConvertToString(model.boneNames[b]));
        return names;
    }

    std::vector<std::string> GetModelBones(std::string_view model)
    {
        // the assets come and go with packs and zones, and the asset table changes while they load
        const auto assets = ContentPacks::TryReadAssets();
        if (!assets.owns_lock() || ContentPacks::IsLoading())
            return {};
        const auto found =
            static_cast<const XModel*>(ContentPacks::FindAsset(ASSET_TYPE_XMODEL, std::string(model).c_str()));
        return found && found->boneNames ? GetBoneNames(*found) : std::vector<std::string>{};
    }

    Types::AnimationPose GetRestPose(const XModel& model)
    {
        Types::AnimationPose pose{.names = GetBoneNames(model), .duration = 0.0f};
        PoseLogic::Compose(GetRestBones(model), pose);
        return pose;
    }

    std::optional<glm::vec3> GetRestPosition(const DObj& obj, std::uint32_t bone)
    {
        std::uint32_t first = 0;
        for (int i = 0; i < obj.numModels; ++i)
        {
            const auto model = obj.models[i];
            if (bone < first + model->numBones)
            {
                const auto index = bone - first;
                if (index < model->numRootBones)
                    return glm::vec3(0.0f);

                const auto& trans = model->trans[index - model->numRootBones];
                return glm::vec3(trans[0], trans[1], trans[2]);
            }
            first += model->numBones;
        }
        return std::nullopt;
    }

    std::optional<SpineRig> FindSpineRig(const XModel& model)
    {
        const auto find = [&](std::string_view name) -> std::int32_t {
            for (std::uint32_t b = model.numRootBones; b < model.numBones; ++b)
                if (Functions::SL_ConvertToString(model.boneNames[b]) == name)
                    return static_cast<std::int32_t>(b);
            return -1;
        };
        const auto parentOf = [&](std::int32_t bone) { return bone - model.parentList[bone - model.numRootBones]; };
        const auto rest = [&](std::int32_t bone) {
            if (bone < model.numRootBones)
                return glm::quat(1, 0, 0, 0);
            const auto& q = model.quats[bone - model.numRootBones];
            return ToQuat(glm::vec4(q[0], q[1], q[2], q[3]));
        };
        const auto stabilizer = find("torso_stabilizer");
        const auto spine = find("j_spinelower");
        if (stabilizer < 0 || spine < 0 || parentOf(spine) != stabilizer || parentOf(stabilizer) < 0)
            return std::nullopt;
        // (a campaign body on a player hangs it from j_mainroot, pelvis left out at rest, see Actors::GetPlayerBody)
        const auto pelvis = parentOf(stabilizer);
        const bool hasPelvis = Functions::SL_ConvertToString(model.boneNames[pelvis]) == std::string_view("pelvis");
        SpineRig rig{.bone = static_cast<std::uint32_t>(stabilizer),
                     .rest = rest(stabilizer),
                     .pelvis = hasPelvis ? rest(pelvis) : glm::quat(1, 0, 0, 0),
                     .spineAt = glm::make_vec3(model.trans[spine - model.numRootBones])};
        rig.lined = PoseLogic::LineUpSpine(rig.pelvis, rig.rest, rig.spineAt);
        return rig;
    }

    PoseLogic::SpineKind GetSpineKind(const XAnimParts& parts)
    {
        std::vector<std::string_view> names;
        for (std::uint32_t i = 0; parts.names && i < parts.boneCount[PART_TYPE_ALL]; ++i)
            names.push_back(Functions::SL_ConvertToString(parts.names[i]));
        return PoseLogic::GetSpineKind(names);
    }

    PoseLogic::AnimationRig ClassifyAnimation(const XAnimParts& parts)
    {
        std::vector<std::string_view> names;
        for (std::uint32_t i = 0; parts.names && i < parts.boneCount[PART_TYPE_ALL]; ++i)
            names.push_back(Functions::SL_ConvertToString(parts.names[i]));
        return PoseLogic::ClassifyAnimation(names);
    }

    std::array<std::int16_t, 4> ToShorts(glm::quat rotation)
    {
        rotation = glm::normalize(rotation);
        const auto toShort = [](float value) { return static_cast<std::int16_t>(std::lround(value * 32767.0f)); };
        return {toShort(rotation.x), toShort(rotation.y), toShort(rotation.z), toShort(rotation.w)};
    }

    std::int32_t FindBone(const XModel& model, std::string_view name)
    {
        for (std::uint32_t b = 0; b < model.numBones; ++b)
            if (Functions::SL_ConvertToString(model.boneNames[b]) == name)
                return static_cast<std::int32_t>(b);
        return -1;
    }

    // the bone a model's bone hangs from, -1 for a root bone
    std::int32_t GetParent(const XModel& model, std::int32_t bone)
    {
        return bone < model.numRootBones ? -1 : bone - model.parentList[bone - model.numRootBones];
    }

    // a bone at rest relative to the bone it hangs from (a root bone at the model's origin), as GetRestBones has it
    PoseLogic::LocalBone GetRestBone(const XModel& model, std::int32_t bone)
    {
        if (bone < model.numRootBones)
            return {};
        const auto i = bone - model.numRootBones;
        const auto& q = model.quats[i];
        return {.rotation = ToQuat(glm::vec4(q[0], q[1], q[2], q[3])),
                .translation = glm::make_vec3(model.trans[i]),
                .parent = GetParent(model, bone)};
    }

    std::optional<PoseLogic::SpineChain> FindSpineChain(const XModel& model)
    {
        const auto root = FindBone(model, "j_mainroot"), pelvis = FindBone(model, "pelvis");
        const auto stabilizer = FindBone(model, "torso_stabilizer"), spine = FindBone(model, "j_spinelower");
        if (root < 0 || pelvis < 0 || stabilizer < 0 || spine < 0 || GetParent(model, pelvis) != root ||
            GetParent(model, stabilizer) != pelvis || GetParent(model, spine) != stabilizer)
            return std::nullopt;
        return PoseLogic::SpineChain{.pelvis = GetRestBone(model, pelvis),
                                     .stabilizer = GetRestBone(model, stabilizer),
                                     .spine = GetRestBone(model, spine)};
    }

    std::optional<PoseLogic::SpineChain> FitSpineChain(const XModel& body)
    {
        const auto chain = GetSpineChain();
        const auto root = FindBone(body, "j_mainroot"), spine = FindBone(body, "j_spinelower");
        if (!chain || root < 0 || spine < 0 || GetParent(body, spine) != root ||
            FindBone(body, "torso_stabilizer") >= 0)
            return std::nullopt;
        auto fitted = PoseLogic::FitSpineChain(*chain, GetRestBone(body, spine));
        const auto shorts = ToShorts(fitted.spine.rotation);
        fitted.spine.rotation = ToQuat(glm::vec4(shorts[0], shorts[1], shorts[2], shorts[3]));
        return fitted;
    }

    // A body with other arms than a soldier's is decoded on a soldier's arms and gun tag, as it is drawn holding a gun
    // (see GripLogic, Actors::GetHoldingModel): what a reload's hand does to the magazine is worked out from this
    void FitSoldierArms(const XModel& body, std::vector<PoseLogic::LocalBone>& bones)
    {
        const auto gun = FindBone(body, "tag_weapon_right");
        const auto rest = [&](const char* name) {
            const auto bone = FindBone(body, name);
            return bone >= 0 ? bones[bone].translation : glm::vec3(0.0f);
        };
        const GripLogic::ArmRest left{rest("j_clavicle_le"), rest("j_shoulder_le"), rest("j_elbow_le"), rest("j_wrist_le")};
        const GripLogic::ArmRest right{rest("j_clavicle_ri"), rest("j_shoulder_ri"), rest("j_elbow_ri"), rest("j_wrist_ri")};
        if (gun < 0 || FindBone(body, "j_wrist_le") < 0 ||
            (GripLogic::IsSoldierArm(left, GripLogic::SOLDIER_LEFT_ARM) &&
             GripLogic::IsSoldierArm(right, GripLogic::SOLDIER_RIGHT_ARM) &&
             GripLogic::IsSoldierGunTag(bones[gun].translation)))
            return;

        const auto set = [&](const char* name, const glm::vec3& translation) {
            if (const auto bone = FindBone(body, name); bone >= 0)
                bones[bone].translation = translation;
        };
        set("j_clavicle_le", GripLogic::SOLDIER_LEFT_ARM.clavicle);
        set("j_shoulder_le", GripLogic::SOLDIER_LEFT_ARM.shoulder);
        set("j_elbow_le", GripLogic::SOLDIER_LEFT_ARM.elbow);
        set("j_wrist_le", GripLogic::SOLDIER_LEFT_ARM.wrist);
        set("j_clavicle_ri", GripLogic::SOLDIER_RIGHT_ARM.clavicle);
        set("j_shoulder_ri", GripLogic::SOLDIER_RIGHT_ARM.shoulder);
        set("j_elbow_ri", GripLogic::SOLDIER_RIGHT_ARM.elbow);
        set("j_wrist_ri", GripLogic::SOLDIER_RIGHT_ARM.wrist);
        set("tag_weapon_right", GripLogic::SOLDIER_GUN_TAG);
        const auto& rotation = GripLogic::SOLDIER_GUN_TAG_ROTATION;
        bones[gun].rotation = ToQuat(glm::vec4(rotation[0], rotation[1], rotation[2], rotation[3]));
    }

    Types::AnimationPose PoseAt(const DecodedAnimation& anim, const XModel& body, float time)
    {
        const float frame = std::clamp(time, 0.0f, 1.0f) * anim.numFrames;

        auto bones = GetRestBones(body);
        FitSoldierArms(body, bones);
        for (std::uint32_t b = 0; b < body.numBones; ++b)
        {
            if (anim.rotations[b])
                bones[b].rotation = ToQuat(Sample(*anim.rotations[b], frame, true));
            if (anim.translations[b])
                bones[b].translation += glm::vec3(Sample(*anim.translations[b], frame, false));
        }
        // lined up as the made up animation of it in actors' trees has it (see Actors.cpp)
        if (anim.linedSpine)
        {
            auto& stabilizer = bones[anim.linedSpine->bone];
            stabilizer.rotation = anim.linedSpine->lined.rotation;
            stabilizer.translation += anim.linedSpine->lined.offset;
        }
        // on the spine rig (see Actors.cpp): j_spinelower hangs from the rig's stabilizer, which hangs from j_mainroot
        if (anim.spineChain)
        {
            const auto keys = [&](const std::optional<Track>& rotation, const std::optional<Track>& translation) {
                PoseLogic::BoneKeys animated;
                if (rotation)
                    animated.rotation = ToQuat(Sample(*rotation, frame, true));
                if (translation)
                    animated.offset = glm::vec3(Sample(*translation, frame, false));
                return animated;
            };
            auto& spine = bones[anim.spineBone];
            const auto parent = spine.parent;
            spine = PoseLogic::ComposeSpine(*anim.spineChain, keys(anim.rigRotations[0], anim.rigTranslations[0]),
                                            keys(anim.rigRotations[1], anim.rigTranslations[1]),
                                            keys(anim.rotations[anim.spineBone], anim.translations[anim.spineBone]));
            spine.parent = parent;
        }

        Types::AnimationPose pose{.names = anim.boneNames, .duration = anim.duration};
        PoseLogic::Compose(bones, pose);

        // Some tags of the body hang far off a bone with nothing below them, such as ones placed by inverse
        // kinematics, and would draw as long lines out of the body. They are left out.
        constexpr float HELPER_TAG_DISTANCE = 25.0f;
        std::vector<bool> hasChildren(body.numBones);
        for (const auto parent : pose.parents)
        {
            if (parent >= 0)
                hasChildren[parent] = true;
        }
        for (std::uint32_t b = body.numRootBones; b < body.numBones; ++b)
        {
            const auto& t = body.trans[b - body.numRootBones];
            if (!hasChildren[b] && glm::length(glm::vec3(t[0], t[1], t[2])) > HELPER_TAG_DISTANCE)
                pose.parents[b] = -1;
        }
        return pose;
    }
}  // namespace IWXMVM::T5::AnimationPose
