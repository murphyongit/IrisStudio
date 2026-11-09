#include "StdInclude.hpp"
#include "Functions.hpp"

#include "Addresses.hpp"

namespace IWXMVM::T5::Functions
{
    Structures::dvar_t* FindDvar(const std::string_view name)
    {
        // dvars are looked up by a case-insensitive hash of their name
        const std::string nameString{name};

        typedef int(__cdecl * Com_HashString_t)(const char* string, int maxLen);
        const auto Com_HashString = reinterpret_cast<Com_HashString_t>(GetGameAddresses().Com_HashString());
        const int hash = Com_HashString(nameString.c_str(), 0);

        const auto Dvar_FindMalleableVar = GetGameAddresses().Dvar_FindMalleableVar();
        Structures::dvar_t* dvar;

        __asm
        {
            push esi
            mov esi, hash
            call Dvar_FindMalleableVar
            mov dvar, eax
            pop esi
        }

        return dvar;
    }

    std::string GetFilePath(const std::string_view demoName)
    {
        // the demo command opens files through FS_SV_FOpenFileRead with the "demos" directory, which tries
        // fs_homepath before fs_basepath
        for (const auto dvarName : {"fs_homepath", "fs_basepath"})
        {
            const auto dvar = FindDvar(dvarName);
            if (!dvar || !dvar->current.string)
                continue;

            auto path = std::filesystem::path(dvar->current.string) / "demos" / demoName;
            if (std::filesystem::exists(path))
                return path.string();
        }

        return "";
    }

    void Cbuf_AddText(std::string command)
    {
        LOG_DEBUG("Executing command \"{0}\"", command);

        command.append("\n");

        typedef void(__cdecl * Cbuf_AddText_t)(int localClientNum, const char* text);
        const auto Cbuf_AddText = reinterpret_cast<Cbuf_AddText_t>(GetGameAddresses().Cbuf_AddText());

        Cbuf_AddText(0, command.c_str());
    }

    void CG_ExecuteNewServerCommands(int localClientNum, int latestSequence)
    {
        typedef void(__cdecl * CG_ExecuteNewServerCommands_t)(int localClientNum, int latestSequence);
        const auto CG_ExecuteNewServerCommands =
            reinterpret_cast<CG_ExecuteNewServerCommands_t>(GetGameAddresses().CG_ExecuteNewServerCommands());

        CG_ExecuteNewServerCommands(localClientNum, latestSequence);
    }

    void CL_FirstSnapshot(int localClientNum)
    {
        typedef void(__cdecl * CL_FirstSnapshot_t)(int localClientNum);
        const auto CL_FirstSnapshot = reinterpret_cast<CL_FirstSnapshot_t>(GetGameAddresses().CL_FirstSnapshot());

        CL_FirstSnapshot(localClientNum);
    }

    void CG_ResetEntity(int localClientNum, Structures::centity_t* cent)
    {
        typedef void(__cdecl * CG_ResetEntity_t)(int localClientNum, Structures::centity_t* cent, bool always);
        const auto CG_ResetEntity = reinterpret_cast<CG_ResetEntity_t>(GetGameAddresses().CG_ResetEntity());

        CG_ResetEntity(localClientNum, cent, true);
    }

    void FX_KillEffect(int localClientNum, std::int32_t handle, bool immediately)
    {
        typedef void(__cdecl * FX_KillEffect_t)(int localClientNum, std::int32_t handle, int immediately);
        reinterpret_cast<FX_KillEffect_t>(GetGameAddresses().FX_KillEffect())(localClientNum, handle,
                                                                                immediately ? 1 : 0);
    }

    // A marks system (see FX_InitMarksSystem, 0x51DD70): 0x400 triangle groups of 0x18 bytes from +0x6810, the free
    // ones linked through their first field from the head at +0xC08
    constexpr std::size_t MARKS_SYSTEM_SIZE = 0x26028;
    constexpr std::size_t MARKS_FREE_TRI_GROUPS = 0xC08;
    constexpr std::size_t MARKS_TRI_GROUPS = 0x6810;
    constexpr std::size_t MARKS_TRI_GROUP_SIZE = 0x18;
    constexpr std::size_t MARKS_TRI_GROUP_COUNT = 0x400;

    std::uint8_t* GetMarksSystem(int localClientNum)
    {
        const auto systems = *reinterpret_cast<std::uint8_t**>(GetGameAddresses().fx_marksSystems());
        return systems ? systems + localClientNum * MARKS_SYSTEM_SIZE : nullptr;
    }

    void FX_ClearMarks(int localClientNum)
    {
        typedef void(__cdecl * FX_InitMarksSystem_t)(void* marksSystem);
        if (const auto system = GetMarksSystem(localClientNum))
            reinterpret_cast<FX_InitMarksSystem_t>(GetGameAddresses().FX_InitMarksSystem())(system);
    }

    std::int32_t FX_SpawnWorldEffect(void* def, const float (&origin)[3], const float (&axis)[3][3],
                                     std::int32_t msecBegin, bool keep, std::uint32_t markEntnum)
    {
        const auto system = *reinterpret_cast<void**>(GetGameAddresses().fx_system());
        if (!def || !system)
            return 0;
        if (!keep)
        {
            typedef std::int32_t(__cdecl * FX_PlayOrientedEffect_t)(std::int32_t localClientNum, void* def,
                                                                    std::int32_t msecBegin, const float* origin,
                                                                    const float(*axis)[3], std::uint32_t markEntnum);
            return reinterpret_cast<FX_PlayOrientedEffect_t>(GetGameAddresses().FX_PlayOrientedEffect())(
                0, def, msecBegin, origin, axis, markEntnum);
        }
        typedef std::int32_t(__cdecl * FX_SpawnEffect_t)(
            void* system, void* def, std::int32_t msecBegin, const float* origin, const float(*axis)[3],
            std::uint32_t dobjHandle, std::uint32_t bone, std::uint32_t sortOrder, std::uint32_t owner,
            std::uint32_t markEntnum, const float* offset);
        // (no DObj, bone, sort order or owner: one of the world's, as the game's own map effects are)
        constexpr std::uint32_t NO_DOBJ = 0x7FF, NO_BONE = 0x1FF, NO_SORT = 0xFF, NO_OWNER = 0xFFFF;
        static constexpr float NO_OFFSET[3] = {};
        return reinterpret_cast<FX_SpawnEffect_t>(GetGameAddresses().FX_SpawnEffect())(
            system, def, msecBegin, origin, axis, NO_DOBJ, NO_BONE, NO_SORT, NO_OWNER, markEntnum, NO_OFFSET);
    }

    void SND_AddLoopEmitter(std::uint32_t aliasHash, const float (&origin)[3])
    {
        typedef void(__cdecl * SND_LoopEmitter_t)(std::uint32_t aliasHash, const float* origin);
        reinterpret_cast<SND_LoopEmitter_t>(GetGameAddresses().SND_AddLoopEmitter())(aliasHash, origin);
    }

    void SND_RemoveLoopEmitter(std::uint32_t aliasHash, const float (&origin)[3])
    {
        typedef void(__cdecl * SND_LoopEmitter_t)(std::uint32_t aliasHash, const float* origin);
        reinterpret_cast<SND_LoopEmitter_t>(GetGameAddresses().SND_RemoveLoopEmitter())(aliasHash, origin);
    }

    void CG_GetImpactEffect(std::uint32_t weaponIndex, std::uint32_t surface, std::uint32_t flags, void*& effect,
                            std::uint32_t& sound)
    {
        typedef void(__cdecl * CG_GetImpactEffect_t)(std::uint32_t weaponIndex, std::uint32_t surface, std::uint32_t flags,
                                                     void** effect, std::uint32_t* sound);
        effect = nullptr;
        sound = 0;
        reinterpret_cast<CG_GetImpactEffect_t>(GetGameAddresses().CG_GetImpactEffect())(weaponIndex, surface, flags,
                                                                                        &effect, &sound);
    }

    std::int32_t FX_PlayEffectOnBone(void* def, std::int32_t msecBegin, std::int32_t handle, std::uint32_t bone,
                                     const float (&origin)[3], const float (&axis)[3][3])
    {
        typedef std::int32_t(__cdecl * FX_PlayEffectOnBone_t)(std::int32_t localClientNum, void* def, std::int32_t msecBegin,
                                                              std::int32_t handle, std::uint32_t bone, const float* origin,
                                                              const float(*axis)[3]);
        return reinterpret_cast<FX_PlayEffectOnBone_t>(GetGameAddresses().FX_PlayEffectOnBone())(0, def, msecBegin, handle,
                                                                                                 bone, origin, axis);
    }

    void CG_ApplyVisionConfigStrings(int localClientNum)
    {
        typedef void(__cdecl * CG_VisionConfigStringModified_t)(int localClientNum);
        reinterpret_cast<CG_VisionConfigStringModified_t>(GetGameAddresses().CG_VisionNakedConfigStringModified())(
            localClientNum);
        reinterpret_cast<CG_VisionConfigStringModified_t>(GetGameAddresses().CG_VisionNightConfigStringModified())(
            localClientNum);
    }

    std::size_t FX_CountMarkTriGroups(int localClientNum)
    {
        const auto system = GetMarksSystem(localClientNum);
        if (!system)
            return 0;
        const auto first = system + MARKS_TRI_GROUPS;
        const auto end = first + MARKS_TRI_GROUP_COUNT * MARKS_TRI_GROUP_SIZE;
        std::size_t free = 0;
        for (auto group = *reinterpret_cast<std::uint8_t**>(system + MARKS_FREE_TRI_GROUPS);
             group >= first && group < end && free < MARKS_TRI_GROUP_COUNT;
             group = *reinterpret_cast<std::uint8_t**>(group))
            ++free;
        return MARKS_TRI_GROUP_COUNT - free;
    }

    Structures::DObj* Com_GetClientDObj(int handle, int localClientNum)
    {
        typedef Structures::DObj*(__cdecl * Com_GetClientDObj_t)(int handle, int localClientNum);
        const auto Com_GetClientDObj = reinterpret_cast<Com_GetClientDObj_t>(GetGameAddresses().Com_GetClientDObj());

        return Com_GetClientDObj(handle, localClientNum);
    }

    std::uint16_t SL_GetString(const char* string)
    {
        // the arguments the game creates its own constant strings (tag_origin, tag_weapon...) with
        typedef unsigned int(__cdecl * SL_GetString_t)(const char* string, int user, int instance);
        const auto SL_GetString = reinterpret_cast<SL_GetString_t>(GetGameAddresses().SL_GetString());

        return static_cast<std::uint16_t>(SL_GetString(string, 1, 0));
    }

    std::size_t GetWeaponVariantCount()
    {
        typedef std::int32_t(__cdecl * BG_GetNumWeapons_t)();
        const auto count = reinterpret_cast<BG_GetNumWeapons_t>(GetGameAddresses().BG_GetNumWeapons())();
        return std::clamp<std::size_t>(count, 0, Structures::MAX_WEAPON_VARIANTS);
    }

    std::string_view SL_ConvertToString(std::uint16_t string)
    {
        typedef const char*(__cdecl * SL_ConvertToString_t)(unsigned int string, int instance);
        const auto SL_ConvertToString =
            reinterpret_cast<SL_ConvertToString_t>(GetGameAddresses().SL_ConvertToString());

        const auto text = string ? SL_ConvertToString(string, 0) : nullptr;
        return text ? std::string_view(text) : std::string_view();
    }

    std::optional<std::uint8_t> DObjGetBoneIndex(Structures::DObj* obj, std::uint16_t name, int modelIndex)
    {
        typedef int(__cdecl * DObjGetBoneIndex_t)(Structures::DObj * obj, unsigned int name, std::uint8_t* index,
                                                  int modelIndex);
        const auto DObjGetBoneIndex = reinterpret_cast<DObjGetBoneIndex_t>(GetGameAddresses().DObjGetBoneIndex());

        // 0xFE asks for a lookup, like the game does; -1 searches every model of the DObj
        std::uint8_t index = 0xFE;
        if (!DObjGetBoneIndex(obj, name, &index, modelIndex))
            return std::nullopt;

        return index;
    }

    Structures::cpose_t* CG_GetPose(int localClientNum, int handle)
    {
        typedef Structures::cpose_t*(__cdecl * CG_GetPose_t)(int localClientNum, int handle);
        const auto CG_GetPose = reinterpret_cast<CG_GetPose_t>(GetGameAddresses().CG_GetPose());

        return CG_GetPose(localClientNum, handle);
    }

    bool CG_DObjGetWorldTagMatrix(Structures::cpose_t* pose, Structures::DObj* obj, std::uint16_t tagName,
                                  float (&tagMat)[3][3], float (&origin)[3])
    {
        typedef int(__cdecl * CG_DObjGetWorldTagMatrix_t)(Structures::cpose_t * pose, Structures::DObj * obj,
                                                          unsigned int tagName, float(*tagMat)[3], float* origin);
        const auto CG_DObjGetWorldTagMatrix =
            reinterpret_cast<CG_DObjGetWorldTagMatrix_t>(GetGameAddresses().CG_DObjGetWorldTagMatrix());

        return CG_DObjGetWorldTagMatrix(pose, obj, tagName, tagMat, origin) != 0;
    }

    float XAnimGetTime(Structures::XAnimTree_s* tree, std::uint32_t animIndex)
    {
        typedef float(__cdecl * XAnimGetTime_t)(Structures::XAnimTree_s * tree, unsigned int animIndex);
        const auto XAnimGetTime = reinterpret_cast<XAnimGetTime_t>(GetGameAddresses().XAnimGetTime());

        return XAnimGetTime(tree, animIndex);
    }

    void XAnimGetRelDelta(Structures::XAnim_s* anims, std::uint32_t animIndex, float (&rot)[2], float (&trans)[3],
                          float time1, float time2)
    {
        typedef void(__cdecl * XAnimGetRelDelta_t)(Structures::XAnim_s * anims, unsigned int animIndex, float* rot,
                                                   float* trans, float time1, float time2);
        const auto XAnimGetRelDelta = reinterpret_cast<XAnimGetRelDelta_t>(GetGameAddresses().XAnimGetRelDelta());

        XAnimGetRelDelta(anims, animIndex, rot, trans, time1, time2);
    }

    bool BG_IsRagdollTrajectory(const Structures::trajectory_t* trajectory)
    {
        typedef bool(__cdecl * BG_IsRagdollTrajectory_t)(const Structures::trajectory_t* trajectory);
        const auto BG_IsRagdollTrajectory =
            reinterpret_cast<BG_IsRagdollTrajectory_t>(GetGameAddresses().BG_IsRagdollTrajectory());

        return BG_IsRagdollTrajectory(trajectory);
    }

    void CG_LocationalTrace(Structures::trace_t* result, const float (&start)[3], const float (&end)[3],
                            std::int32_t skipEntity, std::int32_t mask, bool playerShot)
    {
        typedef void(__cdecl * CG_LocationalTrace_t)(Structures::trace_t * result, const float* start, const float* end,
                                                     int skipEntity, int mask, int, int);
        const auto CG_LocationalTrace =
            reinterpret_cast<CG_LocationalTrace_t>(GetGameAddresses().CG_LocationalTrace());

        CG_LocationalTrace(result, start, end, skipEntity, mask, playerShot ? 1 : 0, 0);
    }

    void DObjSetLocalTag(Structures::DObj* obj, int* partBits, std::uint32_t boneIndex, const float (&trans)[3],
                         const float (&angles)[3])
    {
        typedef void(__cdecl * DObjSetLocalTag_t)(Structures::DObj * obj, int* partBits, unsigned int boneIndex,
                                                  const float* trans, const float* angles);
        const auto DObjSetLocalTag = reinterpret_cast<DObjSetLocalTag_t>(GetGameAddresses().DObjSetLocalTag());

        DObjSetLocalTag(obj, partBits, boneIndex, trans, angles);
    }
}  // namespace IWXMVM::T5::Functions
