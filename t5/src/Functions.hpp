#pragma once
#include "Structures.hpp"

namespace IWXMVM::T5::Functions
{
    Structures::dvar_t* FindDvar(const std::string_view name);

    std::string GetFilePath(const std::string_view demoName);

    void Cbuf_AddText(std::string command);

    void CG_ExecuteNewServerCommands(int localClientNum, int latestSequence);

    void CL_FirstSnapshot(int localClientNum);

    // Frees what the client keeps for an entity: its ragdoll, physics objects and the memory of its effects. The game
    // calls it when an entity goes away; doing nothing for an entity that has none of these.
    void CG_ResetEntity(int localClientNum, Structures::centity_t* cent);

    // stops an effect at once (or, not immediately, stops it spawning: what it spawned plays out, as smoke drifting
    // off); a handle of an effect that is gone does nothing
    void FX_KillEffect(int localClientNum, std::int32_t handle, bool immediately = true);

    // Spawns an effect at a point of the world facing the axes (it fires along x), from msecBegin of the client's time
    // (in the past: as far into its life), as the game's map effects are (no entity, bone or mark): kept until
    // FX_KillEffect (FX_SpawnEffect), or let go of so it ends by itself, a looping one once its looping life is over
    // (FX_PlayOrientedEffect, as the client script's playfx). Its handle, 0 for none.
    // markEntnum: what its marks (decals) go on, ENTITYNUM_WORLD for the world's surfaces; none leaves no mark.
    std::int32_t FX_SpawnWorldEffect(void* def, const float (&origin)[3], const float (&axis)[3][3],
                                     std::int32_t msecBegin, bool keep,
                                     std::uint32_t markEntnum = Structures::ENTITYNUM_NONE);

    // a looping sound at a point of the world, from its alias' hash, until it is removed from that point
    void SND_AddLoopEmitter(std::uint32_t aliasHash, const float (&origin)[3]);
    void SND_RemoveLoopEmitter(std::uint32_t aliasHash, const float (&origin)[3]);

    // What a bullet of a weapon (by its index) plays where it hits a surface, as the map's impact table has it: the
    // effect, null for none, and the sound's alias hash, 0 for none. flags: ShotLogic::IMPACT_FLAG_...
    void CG_GetImpactEffect(std::uint32_t weaponIndex, std::uint32_t surface, std::uint32_t flags, void*& effect,
                            std::uint32_t& sound);

    // Plays an effect at a point of the world, kept on a bone of a DObj from there on (a bullet's blood), by the game's
    // handle of the DObj and the bone's index in it. Its handle, 0 for none.
    std::int32_t FX_PlayEffectOnBone(void* def, std::int32_t msecBegin, std::int32_t handle, std::uint32_t bone,
                                     const float (&origin)[3], const float (&axis)[3][3]);

    // removes every impact mark (decal) of a local client
    void FX_ClearMarks(int localClientNum);

    // sets the naked and night visions as the configstrings say, as the client does as they change or the map starts
    void CG_ApplyVisionConfigStrings(int localClientNum);

    // how many of the marks system's triangle groups are in use (about one per mark), for the harness
    std::size_t FX_CountMarkTriGroups(int localClientNum);

    Structures::DObj* Com_GetClientDObj(int handle, int localClientNum);

    // the script string the engine uses for this name, e.g. for bone names; the string stays allocated
    std::uint16_t SL_GetString(const char* string);
    // the text of a script string; empty for none
    std::string_view SL_ConvertToString(std::uint16_t string);

    // the game's weapon variants (bg_weaponVariantDefs), its first entry unused; the table isn't cleared, so what lies
    // past them is the last game's and may be gone
    std::size_t GetWeaponVariantCount();

    // the index of a bone by name in the DObj, in its first model that has one, or only in the model at an index
    std::optional<std::uint8_t> DObjGetBoneIndex(Structures::DObj* obj, std::uint16_t name, int modelIndex = -1);

    // The pose a model is placed from, by DObj handle: an entity's own for an entity, the view model's from 0x600 on.
    // For the player a demo was recorded from, the hook in Hooks/Camera.cpp gives the pose that player is drawn from.
    Structures::cpose_t* CG_GetPose(int localClientNum, int handle);

    bool CG_DObjGetWorldTagMatrix(Structures::cpose_t* pose, Structures::DObj* obj, std::uint16_t tagName,
                                  float (&tagMat)[3][3], float (&origin)[3]);

    // how far into an animation of the tree the model is, from 0 to 1
    float XAnimGetTime(Structures::XAnimTree_s* tree, std::uint32_t animIndex);

    // How far an animation moves and turns the model between two of its times (0 to 1). trans is in the model's own
    // axes (x forward, y left), rot the turn about the up axis as {sin(yaw / 2), cos(yaw / 2)}.
    void XAnimGetRelDelta(Structures::XAnim_s* anims, std::uint32_t animIndex, float (&rot)[2], float (&trans)[3],
                          float time1, float time2);

    // whether the client turns an entity with this trajectory into a ragdoll
    bool BG_IsRagdollTrajectory(const Structures::trajectory_t* trajectory);

    // A line through the world from start to end, which ends where it hits something with the contents of the mask.
    // skipEntity is an entity number, or ENTITYNUM_NONE.
    void CG_LocationalTrace(Structures::trace_t* result, const float (&start)[3], const float (&end)[3],
                            std::int32_t skipEntity, std::int32_t mask, bool playerShot = false);

    // For CG_DoControllers: replaces the animated rotation of a bone with angles and moves it by trans from its rest
    // position, both relative to its parent. Only does something for a bone in partBits that has no offset yet.
    void DObjSetLocalTag(Structures::DObj* obj, int* partBits, std::uint32_t boneIndex, const float (&trans)[3],
                         const float (&angles)[3]);
}  // namespace IWXMVM::T5::Functions
