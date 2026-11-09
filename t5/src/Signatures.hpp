#pragma once
#include "StdInclude.hpp"
#include "Utilities/Signatures.hpp"

// Patterns were taken from BlackOpsMP.exe (PE timestamp 0x4EB1E843) and verified to be unique against the Steam
// executable, the Plutonium t5mp.exe and the in-memory code of a running Plutonium T5 client.
namespace IWXMVM::T5::Signatures
{
    struct T5Addresses
    {
#define Sig IWXMVM::Signatures::Signature < IWXMVM::Signatures::SignatureImpl
#define Lambda IWXMVM::Signatures::Lambdas

        using GAType = IWXMVM::Signatures::GameAddressType;

        Sig("8B 0D ?? ?? ?? ?? 68 ?? ?? ?? ?? 51 C6 05 ?? ?? ?? ?? 00 C7 05", GAType::Data, 2) > d3d9DevicePointer;
        Sig("8B 7C 24 48 3B 3D ?? ?? ?? ?? 0F 85 81 00 00 00 8B 74 24 4C", GAType::Code, -0xD) > MainWndProc;
        Sig("?? ?? ?? ?? ?? 85 DB 75 04 33 C0 5B C3 56 33 F6 80 3B 00 57 BF 05 15 00 00", GAType::Code, 0)
            > Com_HashString;
        Sig("?? ?? ?? ?? ?? B9 01 00 00 00 F0 0F C1 08 8B 15 ?? ?? ?? ?? 85 D2 74 13 6A 00 E8 ?? ?? ?? ?? A1 ?? ?? ?? "
            "?? 83 C4 04 85 C0 75 ED 8B CE 81 E1 FF 03 00 00",
            GAType::Code, 0) > Dvar_FindMalleableVar;
        Sig("?? ?? ?? ?? ?? ?? ?? 8B 4C 24 0C 83 C4 04 8B D1 85 C9 74 1F 8A 01 3C 70", GAType::Code, 0) > Cbuf_AddText;
        Sig("?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 84 C0 74 15 A1 ?? ?? ?? ?? 6A 00 6A 01 68 01 02 00 00", GAType::Code, 0)
            > IN_Frame;
        // jne in CL_KeyEvent that turns any key into escape (ending the demo) while a demo plays
        Sig("83 FD 04 7C 09 83 3D ?? ?? ?? ?? 00 ?? ?? ?? ?? ?? 74 05 83 FD 03 75 24 85 D2 75 20", GAType::Code, 12)
            > CL_KeyEvent_DemoAnyKey;
        Sig("83 25 ?? ?? ?? ?? 01 55 E8", GAType::Data, 2, Lambda::DereferenceAddress) > clientUIActives_keyCatchers;
        // whether the game wants the system cursor shown, written by IN_ShowSystemCursor; the render thread applies
        // it to the cursor's show count every frame
        Sig("8B F0 F7 DE 1B F6 57 8B 3D ?? ?? ?? ?? F7 DE 50 A3 ?? ?? ?? ?? 4E FF D7", GAType::Data, 17,
            Lambda::DereferenceAddress) > systemCursorVisible;

        // demo playback state
        Sig("6A 04 E8 ?? ?? ?? ?? 8B 08 83 C4 04 83 BC 88 84 00 00 00 02 74 3D", GAType::Code, -0x6) > CL_PlayDemo_f;
        Sig("A1 ?? ?? ?? ?? 57 8B 7C 24 0C 83 C0 80 3B F8", GAType::Data, 1, Lambda::DereferenceAddress)
            > clc_serverCommandSequence;
        Sig("A1 ?? ?? ?? ?? 2B 05 ?? ?? ?? ?? 5F 83 E8 05 5E A3", GAType::Data, 17, Lambda::DereferenceAddress)
            > cls_realtime;
        Sig("83 3D ?? ?? ?? ?? 00 75 0E 68 ?? ?? ?? ?? 53 E8", GAType::Data, 2, Lambda::DereferenceAddress) > cl_snap;
        Sig("69 DB 20 27 00 00 81 C3 ?? ?? ?? ?? 83 3B 00", GAType::Data, 8, Lambda::DereferenceAddress) > cl_snapshots;
        Sig("68 ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 68 F0 32 00 00 6A 00 68", GAType::Data, 1, Lambda::DereferenceAddress)
            > cl_gameState;
        Sig("8B 35 ?? ?? ?? ?? 57 8B 7C 24 18 8D 46 24 50", GAType::Data, 2, Lambda::DereferenceAddress) > cgArray;
        Sig("8B 35 ?? ?? ?? ?? 8B 6C 24 14 83 C4 04 39 6E 14", GAType::Data, 2, Lambda::DereferenceAddress) > cgsArray;
        Sig("69 C0 28 03 00 00 03 04 BD ?? ?? ?? ?? D9 40 30", GAType::Data, 9, Lambda::DereferenceAddress)
            > cg_entitiesArray;
        // the pointer to the client's IK states, as the IK's per-frame time step (0x545310) reads it
        Sig("84 C0 74 ?? 8B 44 24 08 8B 04 85 ?? ?? ?? ?? 85 C0 74 ?? 8B 54 24 04 B9 20 00 00 00 83 38 00",
            GAType::Data, 11, Lambda::DereferenceAddress)
            > ik_states;

        // demo compatibility
        // the MSG_ReadLong call in CL_ParseGamestate that reads the client number, which Plutonium redirects
        Sig("53 ?? ?? ?? ?? ?? 83 C4 04 A3 ?? ?? ?? ?? 85 C0 0F 8C ?? ?? ?? ?? 83 F8 20 0F 8D", GAType::Code, 1)
            > CL_ParseGamestate_ReadClientNum;
        Sig("?? ?? ?? ?? ?? 8B 4E 18 8B 46 1C 03 4E 14 57 8D 78 04 3B F9 7F 1B 6A 04", GAType::Code, 0) > MSG_ReadLong;
        // sets up the game rules of a playlist entry, called by CL_DownloadsComplete when a match or demo starts
        Sig("?? ?? ?? ?? ?? 10 81 FF FF 00 00 00 0F 84 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 48 18 55 56 8B 74 24 14",
            GAType::Code, 0) > Playlist_SetupRules;
        Sig("?? ?? ?? ?? ?? 84 C0 75 12 68 ?? ?? ?? ?? 6A 00 E8 ?? ?? ?? ?? 83 C4 08 33 C0 C3 8B 44 24 08 83 F8 40",
            GAType::Code, 0, Lambda::FollowCodeFlow) > LiveStorage_DoWeHavePlaylists;
        Sig("8B 44 24 08 83 F8 40 7D 11 8B C8 69 C9 98 00 00 00 83 B9 ?? ?? ?? ?? 00 75 14", GAType::Data, 19,
            Lambda::DereferenceAddress) > playlists_entryCount;

        // playback
        Sig("83 EC 10 53 56 8B F0 A1 ?? ?? ?? ?? 8B 40 18 57 8B FE 85 C0 74 04", GAType::Code, -0x8)
            > Com_TimeScaleMsec;
        Sig("?? ?? ?? ?? ?? 85 C0 75 02 59 C3 69 C0 1C 01 00 00", GAType::Code, 0) > FS_Read;
        // the call that sets a server's dvar from its config strings, as the client does for all of them when one
        // changes (Plutonium has it go through a function of its own)
        Sig("8D 8E 6A FF FF FF 51 E8 ?? ?? ?? ?? 8B F8 83 C4 04 80 3F 00 74 1F 56 E8 ?? ?? ?? ?? 50 57 ?? ?? ?? ?? ?? 46",
            GAType::Code, 0x1E) > CG_ParseServerDvars_SetDvar;

        // for rewinding
        Sig("?? ?? ?? ?? ?? ?? 02 75 6D 56 E8 ?? ?? ?? ?? 8B 74 24 08 8B 0D", GAType::Code, 0) > CL_FirstSnapshot;
        Sig("A3 ?? ?? ?? ?? A3 ?? ?? ?? ?? ?? ?? ?? ?? ?? 83 C4 20 5E C3", GAType::Code, 10) > Con_TimeJumped_Call;
        Sig("?? ?? ?? ?? ?? ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 8B 6C 24 14 83 C4 04 39 6E 14 7D 2B 57 8B 7C 24 10 FF 46 14",
            GAType::Code, 0) > CG_ExecuteNewServerCommands;
        // frees what the client keeps for an entity (ragdolls, physics objects, effect memory) when it goes away
        Sig("?? ?? ?? ?? ?? 10 33 DB 57 8B 7C 24 10 38 5C 24 18 75 15 57 E8", GAType::Code, 0) > CG_ResetEntity;

        // camera
        // the call in CG_EjectWeaponBrass that decides whether casings come out of the first person gun
        Sig("8B 42 2C 53 51 50 ?? ?? ?? ?? ?? 56 8A D8 E8 ?? ?? ?? ?? 8B 88 98 01 00 00", GAType::Code, 6)
            > CG_EjectWeaponBrass_IsFirstPerson;
        // (localClientNum, entityState_s*, event, weapon)
        Sig("?? ?? ?? ?? ?? 80 78 18 00 0F 84 ?? ?? ?? ?? 55 8B 6C 24 0C 66 83 BD BE 00 00 00 15 0F 8D", GAType::Code,
            0) > CG_EjectWeaponBrass;
        // the client's effects system, as FX_PlayBoltedEffect gets it
        Sig("8B 3D ?? ?? ?? ?? 50 8B 44 24 18 51 8B 4C 24 18 52 50 51 E8", GAType::Data, 2,
            Lambda::DereferenceAddress) > fx_system;
        // the pose of the entity with a DObj handle, which effects attached to a model are placed from
        Sig("?? ?? ?? ?? ?? 00 06 00 00 7D 32 3D 00 04 00 00 8B 4C 24 04 7C 19", GAType::Code, 0) > CG_GetPose;
        Sig("?? ?? ?? ?? ?? 5C 24 08 56 8B 35 ?? ?? ?? ?? 8B 86 ?? ?? ?? ?? F3 0F 11 86", GAType::Code, 0)
            > CG_CalcViewValues;
        Sig("?? ?? ?? ?? ?? 35 ?? ?? ?? ?? F6 86 ?? ?? ?? ?? 04 57 8B 7C 24 24", GAType::Code, 0)
            > CG_ApplyScriptedCamera;
        Sig("?? ?? ?? ?? ?? 35 ?? ?? ?? ?? 8D 86 34 31 04 00 50 8D 8E 30 B7 05 00 51 F3 0F 11 86 38 B7 05 00",
            GAType::Code, 0) > CG_SetViewFromViewAngles;
        // The frame's vision (localClientNum, bool extraCam): picks the vision mode from the demo player's state (flare,
        // infrared, the Valkyrie's tvguided, night, else naked; the camera spike's for the extra camera) and copies
        // its current settings into the refdef. And the frame's screen filter passes (localClientNum, int extraCam):
        // the tvguided, infrared and scope filters of the player's state, then the client scripts'
        Sig("?? ?? ?? ?? ?? ?? ?? 56 8B 35 ?? ?? ?? ?? 7C ?? A1 ?? ?? ?? ?? 80 78 18 00 75 ?? F3 0F 10 86 48 0C 04 00",
            GAType::Code, 0) > CG_VisionSetFrame;
        Sig("?? ?? ?? ?? ?? 1D ?? ?? ?? ?? 55 8B AC 24 88 00 00 00 56 57 6A 01 55 E8 ?? ?? ?? ?? D9 E8 D9 EE 83 C4 08 84 "
            "C0 74 ?? 8B 83 B8 00 07 00",
            GAType::Code, 0) > CG_ScreenFilters;
        // the main thread waits here for the render thread to finish the frames it was handed
        Sig("?? ?? ?? ?? ?? 56 85 C0 74 0A 83 78 18 00 0F 85 80 00 00 00 8B 0D ?? ?? ?? ?? 64 8B 15 2C 00 00 00",
            GAType::Code, 0) > R_SyncRenderThread;

        // bone camera
        Sig("?? ?? ?? ?? ?? 44 24 04 69 C9 01 06 00 00 03 C1 0F B7 04 45 ?? ?? ?? ?? 66 85 C0 74", GAType::Code, 0)
            > Com_GetClientDObj;
        // the other function with this start is CG_DObjGetWorldTagPos, which differs after the bone lookup
        Sig("?? ?? ?? ?? ?? 56 57 8B 7C 24 14 6A FF 8D 44 24 0F 50 51 57 C6 44 24 1B FE E8 ?? ?? ?? ?? 83 C4 10 85 C0 "
            "74 27 0F B6 74 24 0B 8B 54 24 10 56 57 52 E8 ?? ?? ?? ?? 57 E8 ?? ?? ?? ?? 83 C4 10 85 C0 74 09 C1 E6 05 "
            "03 F0 85 F6 75 06 5F 33 C0 5E 59 C3 8B 44 24 1C 50 56 E8",
            GAType::Code, 0) > CG_DObjGetWorldTagMatrix;
        Sig("?? ?? ?? ?? ?? 0F B6 00 3D FF 00 00 00 75 04 33 C0 59 C3 8B 4C 24 08 53 0F B6 59 09", GAType::Code, 0)
            > DObjGetBoneIndex;
        Sig("?? ?? ?? ?? ?? C2 56 8D 70 01 8D 9B 00 00 00 00 8A 08 40 84 C9 75 F9 2B C6 6A 06 40 50 8B 44 24 18 6A 01 "
            "52 50 E8",
            GAType::Code, 0) > SL_GetString;
        // the text of a script string of an instance (0 for the game's), e.g. a notetrack's name
        Sig("?? ?? ?? ?? ?? C0 74 13 8B 4C 24 08 8B 14 8D ?? ?? ?? ?? C1", GAType::Code, 0) > SL_ConvertToString;

        // the script memory tree that the script strings and vectors of each script VM are kept in
        Sig("?? ?? ?? ?? ?? 0C 56 57 8B 7C 24 18 8B C5 E8 ?? ?? ?? ?? 6A", GAType::Code, 0) > MT_AllocIndex;
        Sig("?? ?? ?? ?? ?? 0C 56 57 8B 7C 24 1C 8B C5 E8 ?? ?? ?? ?? 6A", GAType::Code, 0) > MT_FreeIndex;
        // the nodes, and the roots of the trees of free blocks of each size, of the first VM
        Sig("3B 4C 24 20 89 44 24 28 7D 0C 81 C6 ?? ?? ?? ?? 29 44 24 20 EB 0A 81 C6", GAType::Data, 12,
            Lambda::DereferenceAddress) > mt_nodes;
        Sig("69 C0 C0 01 08 00 03 44 24 1C 55 56 8D 34 45 ?? ?? ?? ?? 57 0F B7 3E 85", GAType::Data, 15,
            Lambda::DereferenceAddress) > mt_heads;
        // the game's CRITICAL_SECTIONs, by Sys_TryEnterCriticalSection (its Enter and Leave differ only in the import)
        Sig("8B 44 24 04 8D 04 40 8D 0C C5 ?? ?? ?? ?? 51 FF 15 ?? ?? ?? ?? F7 D8 1B C0 F7 D8 C3", GAType::Data, 10,
            Lambda::DereferenceAddress) > s_criticalSections;

        // HUD
        // gathers the script HUD elements of the snapshot, sorted, into the array in edx and returns how many; used for
        // the 2D HUD and both passes that draw waypoints
        Sig("?? ?? ?? ?? ?? 57 8B 78 2C 85 FF 75 04 33 C0 5F C3 56 33 F6 8D 87 D8 0B", GAType::Code, 0)
            > CG_CollectHudElems;
        // whether a menu is shown, from its flags and the UI visibility bits of the local client; used to paint menus
        Sig("?? ?? ?? ?? ?? F8 8B 45 0C 8B 08 81 EC 14 01 00 00 53 56 8B 75 10 8B 54 8E 54", GAType::Code, 0)
            > Menu_IsVisible;
        // the red that fills the screen for a while after the player is hurt, by how much
        Sig("?? ?? ?? ?? ?? 74 24 20 83 7E 2C 00 0F 84 ?? ?? ?? ?? 83 7E 14 01 0F 84", GAType::Code, 0)
            > CG_DrawFlashDamage;
        // the damage indicators drawn for cg_hudDamageIconTime, outside the menus
        Sig("?? ?? ?? ?? ?? 2A 44 24 28 53 8B 5C 24 28 56 8B 35 ?? ?? ?? ?? 57 8B F8", GAType::Code, 0)
            > CG_DrawDamageDirectionIndicators;
        // "Connection Interrupted" and its icon, while no snapshot came for a while; takes the local client in eax
        Sig("?? ?? ?? ?? ?? F0 56 C7 44 24 10 00 00 00 00 C7 44 24 14 00 00 00 00 E8", GAType::Code, 0)
            > CG_DrawDisconnect;
        // the render thread's handler of the command R_AddCmdDrawText makes (id 0x11 of the render command table at
        // 0xBC26B0); takes the execute state, whose first field points to the command, and moves it on to the next
        Sig("?? ?? ?? ?? ?? 74 24 18 8B 16 F3 0F 10 42 10 F3 0F 59 05 ?? ?? ?? ?? 8D 44 24 18", GAType::Code, 0)
            > RB_DrawText2DCmd;

        // player death animations
        // interpolates an entity of the snapshot, updates its model and hands it to CG_ProcessEntity
        Sig("?? ?? ?? ?? ?? 5C 24 30 81 FB 00 04 00 00 55 56 8B 35 ?? ?? ?? ?? 57 8B 7C 24 38", GAType::Code, 0)
            > CG_AddPacketEntity;
        // XAnimGetWeight differs only in the field it reads
        Sig("?? ?? ?? ?? ?? B7 40 04 66 85 C0 74 2B 8B 54 24 08 0F B7 C8 51 52 E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 16 C1 "
            "E0 06 F3 0F 10 80 50",
            GAType::Code, 0) > XAnimGetTime;
        Sig("?? ?? ?? ?? ?? 4C 24 04 C1 E0 04 8D 44 08 18 83 EC 18 66 83 78 02 00 0F 85 ?? ?? ?? ?? 8B 40 0C 80 78 11 "
            "00 0F 84 ?? ?? ?? ?? D9 44 24 30 0F 57 C0",
            GAType::Code, 0) > XAnimGetRelDelta;
        // sets the bone offsets of a model before its bones are worked out, e.g. the aim of a player's upper body
        Sig("?? ?? ?? ?? ?? 8B 6C 24 24 56 57 8D 44 24 10 50 55 E8 ?? ?? ?? ?? 8B 5C 24 30 0F B6 43 02 48",
            GAType::Code, 0) > CG_DoControllers;
        Sig("?? ?? ?? ?? ?? 81 FF FE 00 00 00 73 46 8B CF 83 E1 1F 8B D7 C1 FA 05 B8 00 00 00 80", GAType::Code, 0)
            > DObjSetLocalTag;
        // works out the rotations of a DObj's bones relative to their parents from its animations (after
        // CG_DoControllers, as DObjCalcSkel's first step), for those asked for that no controller set
        Sig("?? ?? ?? ?? ?? F0 B8 54 78 00 00 E8 ?? ?? ?? ?? 53 56 57 33 C0 89 84 24 44 78 00 00", GAType::Code, 0)
            > DObjCalcAnim;
        // puts a DObj's skeleton together in the model from what DObjCalcAnim worked out, for the bones asked for
        Sig("?? ?? ?? ?? ?? 5C 24 70 55 56 57 8B BC 24 80 00 00 00 8B 2F 8D 73 14 F7 D5 0B 6E 28", GAType::Code, 0)
            > DObjCalcSkel;
        // jne in the code that sets up player and corpse models, which takes the weapon out of a corpse's hands
        Sig("66 83 BE BE 00 00 00 02 ?? ?? ?? ?? ?? 14 0F B7 56 62", GAType::Code, 8) > CG_CorpseWeapon;
        // ragdoll_enable && trType is one of the ragdoll trajectories
        Sig("?? ?? ?? ?? ?? 80 78 18 00 74 14 8B 4C 24 04 8A 01 3C 0C 72 0A 3C 0E 77 06", GAType::Code, 0)
            > BG_IsRagdollTrajectory;
        // the client's trace (result, start, end, entity to skip, contents mask, 0, 0), as the client script's
        // bullettrace calls it
        Sig("?? ?? ?? ?? ?? ?? ?? ?? 57 56 8D 4C 24 0C E8 ?? ?? ?? ?? 8B 44 24 4C", GAType::Code, 0) > CG_LocationalTrace;

        // passes
        // sets up the material of a draw surface (the 64-bit sort key is the last two stack arguments) for the
        // surfaces that use it; returning 0 skips them. Called by the loop that draws every 3D surface.
        Sig("?? ?? ?? ?? ?? ?? 8B 55 10 83 EC 0C 53 56 8B 75 18", GAType::Code, 0) > R_SetupDrawSurfMaterial;
        Sig("8B 0C B5 ?? ?? ?? ?? 5F 5E 5D 5B E9 ?? ?? ?? ?? 83 FE 04", GAType::Data, 3, Lambda::DereferenceAddress)
            > rgp_sortedMaterials;
        // fills the GfxWindowParms in edi from r_fullscreen, r_customMode and r_mode, at most the desktop's size in a
        // window; run again when one of them changes, after which the device is reset to the new size
        Sig("?? ?? ?? ?? ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 88 47 08", GAType::Code, 0) > R_SetWndParms;

        // reading a whole demo for its kills (see DemoScanner.cpp); most are found from their calls in
        // CL_ParseServerMessage, CL_ParseGamestate and CL_ParseSnapshot
        Sig("?? ?? ?? ?? ?? 83 FE 01 74 05 83 FE 05 75 44 E8", GAType::Code, 0) > Com_Error;
        Sig("?? ?? ?? ?? ?? 8B 45 14 2B 45 1C 83 C4 0C 3D 00 00 01 00", GAType::Code, 0, Lambda::FollowCodeFlow)
            > MSG_Init;
        // decompresses the part of a server message after its first four bytes into another buffer
        Sig("?? ?? ?? ?? ?? 83 C4 10 83 7C 24 10 00 89 44 24 24", GAType::Code, 0, Lambda::FollowCodeFlow)
            > MSG_ReadCompressed;
        Sig("?? ?? ?? ?? ?? 83 C4 18 3B C5 75 0C C7 05 ?? ?? ?? ?? FF FF FF FF", GAType::Code, 0,
            Lambda::FollowCodeFlow) > MSG_ReadByte;
        Sig("?? ?? ?? ?? ?? 8B D8 33 ED 55 89 5C 24 1C E8 ?? ?? ?? ?? 8B F0", GAType::Code, 0, Lambda::FollowCodeFlow)
            > MSG_ReadShort;
        Sig("?? ?? ?? ?? ?? 83 C4 10 39 35 ?? ?? ?? ?? 7D 4C", GAType::Code, 0, Lambda::FollowCodeFlow)
            > MSG_ReadString;
        Sig("?? ?? ?? ?? ?? 83 C4 04 85 C0 74 07 47 89 7C 24 10", GAType::Code, 0, Lambda::FollowCodeFlow)
            > MSG_ReadBit;
        Sig("?? ?? ?? ?? ?? 83 C4 08 89 44 24 10 8B F8 85 FF", GAType::Code, 0, Lambda::FollowCodeFlow) > MSG_ReadBits;
        Sig("?? ?? ?? ?? ?? 8B F0 83 C4 08 85 F6 7C 08 81 FE 00 04 00 00", GAType::Code, 0, Lambda::FollowCodeFlow)
            > MSG_ReadEntityIndex;
        Sig("?? ?? ?? ?? ?? 83 C4 20 EB 17 83 F8 03 0F 85 ?? ?? ?? ?? 53", GAType::Code, 0, Lambda::FollowCodeFlow)
            > MSG_ReadDeltaEntity;
        Sig("?? ?? ?? ?? ?? 83 C4 18 56 E8 ?? ?? ?? ?? 8B 0D", GAType::Code, 0, Lambda::FollowCodeFlow)
            > MSG_ReadDeltaPlayerstate;
        // the team scores and bomb timers of a snapshot
        Sig("?? ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? B8 93 24 49 92", GAType::Code, 0, Lambda::FollowCodeFlow)
            > CL_ParseMatchState;
        Sig("?? ?? ?? ?? ?? 83 C4 14 39 2E 75 12 8B 4C 24 14", GAType::Code, 0, Lambda::FollowCodeFlow)
            > CL_ParsePacketClients;
        // whether a gamestate has three more longs after the map and gametype names; called with 4
        // (it starts with a call, so only this call is followed)
        Sig("?? ?? ?? ?? ?? 83 C4 04 84 C0 74 44 39 2D ?? ?? ?? ?? 75 3C", GAType::Code, 0, Lambda::FollowCall)
            > CL_GamestateHasKeys;
        // and only while this is 0
        Sig("39 2D ?? ?? ?? ?? 75 3C 57 E8 ?? ?? ?? ?? 50 E8", GAType::Data, 2, Lambda::DereferenceAddress)
            > CL_GamestateHasKeys_Flag;
        Sig("8B 0C 85 ?? ?? ?? ?? 8B 41 08 C3 CC A1 ?? ?? ?? ?? 8B 40 18", GAType::Data, 3,
            Lambda::DereferenceAddress) > bg_weaponVariantDefs;
        Sig("?? ?? ?? ?? ?? 83 C4 04 89 47 18 85 C0 75 49 8B 15", GAType::Code, 0, Lambda::FollowCodeFlow)
            > SEH_StringEd_GetString;

        // player loadouts
        // the asset hash table (asset entry numbers) and the entries, {type, header, zone, inuse, next, override}, as
        // the listassetpool command walks them
        Sig("0F B7 B0 ?? ?? ?? ?? 85 F6 0F 84 ?? ?? ?? ?? 90 8B 4C 24 18 C1 E6 04 81 C6", GAType::Data, 3,
            Lambda::DereferenceAddress) > db_hashTable;
        Sig("0F B7 B0 ?? ?? ?? ?? 85 F6 0F 84 ?? ?? ?? ?? 90 8B 4C 24 18 C1 E6 04 81 C6 ?? ?? ?? ?? 0F B7 6E 0A",
            GAType::Data, 25, Lambda::DereferenceAddress) > g_assetEntryPool;

        // content packs (single player and Zombies zones loaded for their characters)
        // usercall: the type in eax, the header on the stack; returns the header the asset is known by from then on
        Sig("?? ?? ?? ?? ?? ?? ?? 56 6A 46 89 44 24 08 89 4C 24 0C E8", GAType::Code, 0) > DB_AddXAsset;
        // the index of the zone being loaded, as the asset entries get it
        Sig("8B 56 04 8B 2E 8B 35 ?? ?? ?? ?? A1 ?? ?? ?? ?? 89 54 24 20 89 44 24 18", GAType::Data, 12,
            Lambda::DereferenceAddress) > g_zoneIndex;
        // the loaded zones, ZONE_INFO_SIZE bytes each and starting with the name, as listassetpool prints them; the
        // call after it is DB_GetXAssetName (XAsset*)
        Sig("8D 14 80 C1 E2 04 81 C2 ?? ?? ?? ?? 52 56 47 E8", GAType::Data, 8, Lambda::DereferenceAddress) > g_zoneInfo;
        Sig("8D 14 80 C1 E2 04 81 C2 ?? ?? ?? ?? 52 56 47 ?? ?? ?? ?? ?? 83 C4 04", GAType::Code, 15,
            Lambda::FollowCall) > DB_GetXAssetName;
        // called as DB_LinkXAssetEntry copies a header into its pool
        Sig("88 56 08 C6 46 09 00 66 89 46 0A 66 89 4E 0C ?? ?? ?? ?? ?? 8B 54 24 28", GAType::Code, 15,
            Lambda::FollowCall) > DB_GetXAssetTypeSize;
        // what unloading a zone runs for each asset it registered (per type, may be null), e.g. releasing an image's
        // texture or a model's vertex buffers (Plutonium hooks the call that follows)
        Sig("8B 16 8B 04 95 ?? ?? ?? ?? 85 C0 74 09 8B 4E 04 51", GAType::Data, 5,
            Lambda::DereferenceAddress) > DB_XAssetUnloadHandlers;
        // R_ReleaseOnRenderThread(object) queues a Direct3D object for the render thread to release, as unloading a
        // technique set does off the render thread; R_WaitForRenderThreadQueue() waits until the render thread has run
        // what was queued, as making a model's buffers off the render thread does
        Sig("?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? FF 05 ?? ?? ?? ?? 8B 4C 24 04 8D "
            "04 40 8D 04 C5 ?? ?? ?? ?? C7 00 02 00 00 00 89 48 04",
            GAType::Code, 0) > R_ReleaseOnRenderThread;
        Sig("?? ?? ?? ?? ?? E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C4 04 85 C0 74 16 E8 ?? ?? ?? ?? B9 ?? ?? ?? ?? 83 CA "
            "FF F0 0F C1 11 E9 ?? ?? ?? ?? B8 ?? ?? ?? ?? 83 C9 FF F0 0F C1 08 C3",
            GAType::Code, 0) > R_WaitForRenderThreadQueue;
        // the hash of an asset's name (name, type), as DB_LinkXAssetEntry looks an asset up in db_hashTable
        Sig("8B 1E 53 50 ?? ?? ?? ?? ?? 8B E8 25 FF 7F 00 00", GAType::Code, 4, Lambda::FollowCall) > DB_HashForName;
        // DB_AllocXAssetHeader takes from DB_XAssetPool[type], whose first member is the next free slot
        Sig("8B 0C 85 ?? ?? ?? ?? 8B 14 85 ?? ?? ?? ?? 56 51 FF D2", GAType::Data, 3, Lambda::DereferenceAddress)
            > DB_XAssetPool;
        // gives a model its index buffers (model), as the renderer does for every model once a map has loaded
        // (DB_EnumXAssets(ASSET_TYPE_XMODEL, R_CreateXModelBuffers, 0, 1) when r_loadForRenderer is set)
        Sig("A1 ?? ?? ?? ?? 80 78 18 00 74 13 6A 01 6A 00 68 ?? ?? ?? ?? 6A 05 E8 ?? ?? ?? ?? 83 C4 10 C3", GAType::Data,
            16, Lambda::DereferenceAddress) > R_CreateXModelBuffers;
        // what the loadzone command does around loading a zone: syncing with the render thread before, and refreshing
        // what the client takes from the zones after (the third only when the second returns false)
        Sig("C7 44 24 0C ?? ?? ?? ?? ?? ?? ?? ?? ?? B8 00 00 20 00 6A 01", GAType::Code, 8, Lambda::FollowCall)
            > DB_BeforeLoadZones;
        // DB_LoadXAssets(XZoneInfo* zones, count, sync), for zones of {name, allocFlags, freeFlags}
        Sig("8D 44 24 04 6A 02 50 C7 44 24 10 00 00 00 00 C7 44 24 20 00 00 00 00 ?? ?? ?? ?? ?? 83 C4 0C ?? ?? ?? ?? "
            "?? ?? ?? ?? ?? ?? 84 C0 75 05 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B E5 5D C3",
            GAType::Code, 23, Lambda::FollowCall) > DB_LoadXAssets;
        Sig("8D 44 24 04 6A 02 50 C7 44 24 10 00 00 00 00 C7 44 24 20 00 00 00 00 ?? ?? ?? ?? ?? 83 C4 0C ?? ?? ?? ?? "
            "?? ?? ?? ?? ?? ?? 84 C0 75 05 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B E5 5D C3",
            GAType::Code, 31, Lambda::FollowCall) > DB_AfterLoadZones1;
        Sig("8D 44 24 04 6A 02 50 C7 44 24 10 00 00 00 00 C7 44 24 20 00 00 00 00 ?? ?? ?? ?? ?? 83 C4 0C ?? ?? ?? ?? "
            "?? ?? ?? ?? ?? ?? 84 C0 75 05 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B E5 5D C3",
            GAType::Code, 36, Lambda::FollowCall) > DB_AfterLoadZones2;
        Sig("8D 44 24 04 6A 02 50 C7 44 24 10 00 00 00 00 C7 44 24 20 00 00 00 00 ?? ?? ?? ?? ?? 83 C4 0C ?? ?? ?? ?? "
            "?? ?? ?? ?? ?? ?? 84 C0 75 05 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B E5 5D C3",
            GAType::Code, 45, Lambda::FollowCall) > DB_AfterLoadZones3;
        Sig("8D 44 24 04 6A 02 50 C7 44 24 10 00 00 00 00 C7 44 24 20 00 00 00 00 ?? ?? ?? ?? ?? 83 C4 0C ?? ?? ?? ?? "
            "?? ?? ?? ?? ?? ?? 84 C0 75 05 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 8B E5 5D C3",
            GAType::Code, 50, Lambda::FollowCall) > DB_AfterLoadZones4;
        // the _snprintf call with which the zone loader formats zone/Common/<name>.ff (base path, name, ".ff")
        Sig("56 E8 ?? ?? ?? ?? 50 68 ?? ?? ?? ?? 8D 4C 24 2C 68 00 01 00 00 51 ?? ?? ?? ?? ?? 83 C4 18", GAType::Code,
            22) > DB_OpenZone_FormatCommonPath;
        // The free flags of the zones the client unloads before it frees its own memory as it leaves a demo or game
        // (0x6000000, the menus' zones), in what disconnecting and the theater's demo_play run (see ContentPacks)
        Sig("6A 01 51 C7 44 24 10 00 00 00 00 C7 44 24 14 00 00 00 00 C7 44 24 18 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 0C "
            "84 DB 5B",
            GAType::Data, 23) > UnloadMenuZones_FreeFlags;
        // the call of DB_LoadXAssets with which a map's zones load, the map's own last (allocation flags 0x4000), after
        // an entry that unloads the menus' (see CampaignMaps)
        Sig("E8 ?? ?? ?? ?? 53 45 8D 4C 24 20 55 51 ?? ?? ?? ?? ?? 53 E8", GAType::Code, 13) > LoadLevelZones_LoadXAssets;
        // whether the game owns a map pack, by its DLC bit (2 for the game's own maps), and the game's table of maps
        // ({name, mode, DLC bit, presence}, 0x10 bytes each, to one without a name), which its map lookup starts from
        // (see CampaignMaps)
        Sig("?? ?? ?? ?? ?? 85 C0 74 09 83 78 18 00 74 03 B0 01 C3 56 8B 74 24 08 83 FE 02 75 04 B0 01 5E C3",
            GAType::Code, 0) > Content_IsOwned;
        Sig("A1 ?? ?? ?? ?? 56 33 F6 80 38 00 57 74 30 8B 7C 24 0C", GAType::Data, 1, Lambda::DereferenceAddress) >
            com_mapTable;
        // the lowest number of the IWDs the file system skips in multiplayer (main/iw_36.iwd to iw_41.iwd, single
        // player's Rezurrection files: "Skip DLC5 SP only"), the immediate of its compare (see CampaignMaps)
        Sig("83 F8 ?? ?? ?? ?? 0F 6A 0A 83 C1 03 53 51 E8", GAType::Data, 2) > FS_SkippedIwdsFirst;
        // the server's give (Cmd_Give_f(ent), a client command, cheats on), which lets the weapon look-up register a
        // weapon the map didn't precache, and ConcatArgs(start), the command's arguments from one on, as it reads them
        // (see WeaponZones)
        Sig("?? ?? ?? ?? ?? 0C 56 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 84 ?? ?? ?? ?? 53 55 57 6A 02 E8", GAType::Code, 0)
            > Cmd_Give_f;
        Sig("?? ?? ?? ?? ?? 0C 56 E8 ?? ?? ?? ?? 83 C4 04 85 C0 0F 84 ?? ?? ?? ?? 53 55 57 6A 02 ?? ?? ?? ?? ?? 50",
            GAType::Code, 28,
            Lambda::FollowCall) > ConcatArgs;
        // CG_SetupWeaponConfigString(index): the client registers the weapon a configstring of the weapons (0xB14 +
        // index, "<index> <name> <variants>", read with CL_GetConfigString) names, as a film or game starts and as the
        // server registers one; "Weapon index mismatch" ends the film when the client finds no such weapon
        Sig("?? ?? ?? ?? ?? 14 0B 00 00 50 E8 ?? ?? ?? ?? 83 C4 04 89 44 24 04 85 C0 0F 84", GAType::Code, 0)
            > CG_SetupWeaponConfigString;
        Sig("?? ?? ?? ?? ?? 14 0B 00 00 50 ?? ?? ?? ?? ?? 83 C4 04 89 44 24 04 85 C0 0F 84", GAType::Code, 10,
            Lambda::FollowCall) > CL_GetConfigString;
        // BG_GetNumWeapons, as CG_RegisterWeapon checks a weapon against it (past it the table has the last game's)
        Sig("85 F6 0F 84 ?? ?? ?? ?? 0F 8C ?? ?? ?? ?? ?? ?? ?? ?? ?? 3B F0 0F 83", GAType::Code, 14, Lambda::FollowCall)
            > BG_GetNumWeapons;
        // the weapon option tables' index of a weapon's parent (its weapon def's parent name), -1 for none, as the game
        // checks it as it registers a weapon (0x463740: "Parent weapon '%s' not found for '%s'" ends the film)
        Sig("8B 4E 08 85 C9 74 ?? 8B 41 3C 85 C0 74 ?? 80 38 00 74 ?? 50 ?? ?? ?? ?? ?? 8B 4E 08 83 C4 04 83 F8 FF",
            GAType::Code, 20, Lambda::FollowCall) > BG_FindWeaponOptionsParent;
        // the map and devmap commands of the server (its arguments are the server's command arguments, read as its
        // code starts: their nesting, counts and values)
        Sig("?? ?? ?? ?? ?? 8B 0C 85 ?? ?? ?? ?? 83 EC 4C 83 F9 01 0F 8E ?? ?? ?? ?? 8B 14 85 ?? ?? ?? ?? 56 8B 72 04",
            GAType::Code, 0) > SV_Map_f;
        // the names of the zones that have a patch zone (<name>_patch.ff in zone/Common, found as the game starts: here
        // common_zombie and the Zombies maps), 12 at most, after which DB_LoadXAssets loads that patch when a zone of
        // the name loads with allocation flags 0x40 or 0x800 (the patch's 0x100 or 0x2000; see CampaignMaps)
        Sig("8B 83 ?? ?? ?? ?? 52 50 E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 0A 83 C3 04 83 FB 30", GAType::Data, 2,
            Lambda::DereferenceAddress) > db_patchZoneNames;
        // SND_AddBank adds a sound bank to the sound system's list under critical section 8 (all three, and the lock
        // functions, from its code); the patch function does the same for sound patches
        Sig("56 6A 08 ?? ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 83 C4 04 85 F6 75 13 68 80 00 00 00 56 68 ?? ?? ?? ?? E8",
            GAType::Code, 3, Lambda::FollowCall) > Sys_EnterCriticalSection;
        Sig("56 6A 08 ?? ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 83 C4 04 85 F6 75 13 68 80 00 00 00 56 68 ?? ?? ?? ?? E8",
            GAType::Data, 10, Lambda::DereferenceAddress) > snd_bankCount;
        Sig("56 6A 08 ?? ?? ?? ?? ?? 8B 35 ?? ?? ?? ?? 83 C4 04 85 F6 75 13 68 80 00 00 00 56 68 ?? ?? ?? ?? E8",
            GAType::Data, 28, Lambda::DereferenceAddress) > snd_banks;
        Sig("83 C6 04 83 FE 20 72 E5 5E C7 44 24 04 08 00 00 00 ?? ?? ?? ?? ?? CC", GAType::Code, 17,
            Lambda::FollowCodeFlow) > Sys_LeaveCriticalSection;
        Sig("6A 08 ?? ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C4 04 85 C0 75 24 66 0F EF C0 66 0F D6 05", GAType::Data, 8,
            Lambda::DereferenceAddress) > snd_patchCount;
        Sig("6A 08 ?? ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C4 04 85 C0 75 24 66 0F EF C0 66 0F D6 05 ?? ?? ?? ?? 66",
            GAType::Data, 27, Lambda::DereferenceAddress) > snd_patches;
        // builds a player's model again from its entity state (cent) for each player of a new snapshot
        Sig("?? ?? ?? ?? ?? ?? 56 8B B4 24 14 01 00 00 F6 86 24 03 00 00 02 0F 84 ?? ?? ?? ?? 0F B6 86 BD 02 00 00",
            GAType::Code, 0) > CG_UpdatePlayerModel;
        // copies a snapshot of the client's ring into one of cgame's (localClientNum, number, snapshot_t*)
        Sig("?? ?? ?? ?? ?? 83 EC 08 53 8B 5C 24 14 3B D8 7E 14 68", GAType::Code, 0) > CL_GetSnapshot;

        // CG_ProcessSnapshots(localClientNum): moves the client on to the snapshots of its time, the first thing each
        // frame of the client's game does with its entities
        Sig("?? ?? ?? ?? ?? 35 ?? ?? ?? ?? 57 8B 7C 24 18 8D 46 24 50 8D 4C 24 14 51 33 ED", GAType::Code, 0)
            > CG_ProcessSnapshots;

        // actors (models the client draws on its own)
        // adds the entities of the snapshot and the client's own to the scene (localClientNum)
        Sig("?? ?? ?? ?? ?? ?? ?? ?? 8A 88 C0 31 00 00 0F 57 C0 53 8B 5C", GAType::Code, 0) > CG_AddPacketEntities;
        // XAnimCreateAnims(debugName, size, 0, Alloc): the animations of a tree, to fill with XAnimBlend and XAnimCreate
        Sig("?? ?? ?? ?? ?? 55 56 57 8B 7C 24 18 8D 04 9B C1 E7 04 8D 6C", GAType::Code, 0) > XAnimCreateAnims;
        // XAnimBlend(anims, index, name, firstChild, numChildren, flags) makes an entry a blend of the ones after it
        Sig("?? ?? ?? ?? ?? 8B 54 24 14 53 8B 5C 24 0C 55 8B 6C 24 0C 8B", GAType::Code, 0) > XAnimBlend;
        // XAnimCreate(anims, index, name) makes an entry the animation of that name
        Sig("?? ?? ?? ?? ?? 80 78 18 00 56 8B 74 24 10 75 0E 68 ?? ?? ?? ?? 56", GAType::Code, 0) > XAnimCreate;
        // XAnimCreateTree(anims, Alloc) and XAnimFreeTree(tree, Free(pointer, size, context), context)
        Sig("?? ?? ?? ?? ?? 0C 8B 4C 24 08 66 0F EF C0 66 0F D6 00 83 C4", GAType::Code, 0) > XAnimCreateTree;
        Sig("?? ?? ?? ?? ?? 0F B7 46 04 66 85 C0 74 0D 0F B7 C0 50 56 E8 ?? ?? ?? ?? 83 C4 08 8B 44 24 0C",
            GAType::Code, 0) > XAnimFreeTree;
        // XAnimSetCompleteGoalWeight(obj, index, weight, blendTime, rate, notifyName, notifyType, restart, -1), as the
        // client script's setanim calls it
        Sig("?? ?? ?? ?? ?? ?? ?? ?? 0F 2F 44 24 0C 76 09 0F 57 C0 F3 0F 11 44 24 0C 53 8B", GAType::Code, 0)
            > XAnimSetCompleteGoalWeight;
        // XAnimSetGoalWeight, the same without taking the other entries out, as the client script's setanim calls it
        // without its complete flag
        Sig("?? ?? ?? ?? ?? ?? ?? ?? F3 0F 10 44 24 0C 0F 2F C8 0F 57 C9", GAType::Code, 0) > XAnimSetGoalWeight;
        // XAnimSetTime(tree, index, time from 0 to 1, -1), as the client script's setanimtime calls it
        Sig("?? ?? ?? ?? ?? B7 40 04 66 85 C0 0F 84 ?? ?? ?? ?? 8B 54 24", GAType::Code, 0) > XAnimSetTime;
        // Com_ClientDObjCreate(models, numModels, tree, handle, localClientNum) and Com_SafeClientDObjFree(handle,
        // localClientNum), for the handles Com_GetClientDObj takes
        Sig("?? ?? ?? ?? ?? ?? 0F B6 88 CC 07 04 00 53 8B 5C 24 18 56 57", GAType::Code, 0) > Com_ClientDObjCreate;
        Sig("?? ?? ?? ?? ?? 0F BF 34 45 ?? ?? ?? ?? 85 F6 74 37 33 C9 6A", GAType::Code, 0) > Com_SafeClientDObjFree;
        // the renderer's lights: the spot call, which is the flashlight's (origin, axis,
        // radius, r, g, b); scene.addedLightCount and scene.addedLight
        Sig("?? ?? ?? ?? ?? F8 83 EC 20 A1 ?? ?? ?? ?? 83 78 18 00 56 57 8B 7D 0C", GAType::Code, 0)
            > R_AddSpotLightToScene;
        Sig("8B 1D ?? ?? ?? ?? 83 FB 20 7C 11 6A 0B E8", GAType::Data, 2, Lambda::DereferenceAddress)
            > scene_addedLightCount;
        Sig("68 70 01 00 00 81 C6 ?? ?? ?? ?? 6A 00 43 56", GAType::Data, 7, Lambda::DereferenceAddress)
            > scene_addedLights;
        // the light definitions the add-light calls give their lights; the second, the flashlight's, is never set in
        // multiplayer
        Sig("8B 15 ?? ?? ?? ?? F3 0F 10 45 10 F3 0F 10 4D 1C", GAType::Data, 2, Lambda::DereferenceAddress)
            > omniLightDef;
        Sig("8B 15 ?? ?? ?? ?? D9 44 24 20 F3 0F 10 44 24 20", GAType::Data, 2, Lambda::DereferenceAddress)
            > spotLightDef;
        // R_GetVisibleDLights(visible): the four lights of the view
        Sig("?? ?? ?? ?? ?? ?? 81 EC 80 00 00 00 53 33 DB 33 C0 56 85 D2 7E 23 B9", GAType::Code, 0)
            > R_GetVisibleDLights;
        // R_EmitPointLitSurfs(count in ecx, lights in edx, view, its origin) and R_DrawPointLitSurfs(view in eax,
        // context): the additive pass
        Sig("?? ?? ?? ?? ?? 33 C0 56 57 89 44 24 1C 85 C9 0F 8E ?? ?? ?? ?? 8B 5C 24 30", GAType::Code, 0)
            > R_EmitPointLitSurfs;
        Sig("?? ?? ?? ?? ?? C0 81 EC B4 00 00 00 53 56 57 8B F8 8B B7 30 09 00 00 85 F6", GAType::Code, 0)
            > R_DrawPointLitSurfs;
        // what R_DrawPointLitSurfs draws each light with (a callback: its data has the view, its list and its scissor)
        Sig("?? ?? ?? ?? ?? F8 8B 55 10 83 EC 10 56 57 8B 7D 08 8B 07 0F B6 88", GAType::Code, 0)
            > R_DrawPointLitSurfsCallback;
        // R_SetQuadMesh(mesh in eax, x, y, width, height): the screen rectangle the renderer's passes draw with
        Sig("?? ?? ?? ?? ?? ?? ?? ?? ?? 8B 46 0C 0F 57 C0 F3 0F 10 0D ?? ?? ?? ?? F3 0F 10 64 24 0C F3 0F 10 54 24 10 33 D2",
            GAType::Code, 0) > R_SetQuadMesh;
        // the game's frame data (the front end's), the index among the shadowable lights of the spot that isn't the
        // map's, and the time R_ChooseShadowedLights last ran, which it runs only when it moves
        Sig("8B 15 ?? ?? ?? ?? 8B C8 53 56 8B D9 69 DB E0 38 00 00", GAType::Data, 2, Lambda::DereferenceAddress)
            > frontEndData;
        Sig("A1 ?? ?? ?? ?? 56 8B C8 83 E1 1F BE 01 00 00 00 D3 E6", GAType::Data, 1, Lambda::DereferenceAddress)
            > dynamicSpotIndex;
        Sig("8B 8B 70 01 00 00 55 8D 2C 80 2B 0C ED ?? ?? ?? ?? 8D 2C ED", GAType::Data, 13, Lambda::DereferenceAddress)
            > shadowChooserLastTime;
        // R_AddDObjToScene(obj, pose, entnum, renderFlags, lightingOrigin, 4 floats, -1, -1, 0, 0, 0.0f, 1.0f), as
        // the client adds a script model
        Sig("?? ?? ?? ?? ?? ?? 8B 54 24 0C B8 FF FF 00 00 83 EC 0C 66 39", GAType::Code, 0) > R_AddDObjToScene;
        // CG_AddPlayerWeapon(localClientNum, placement, ps, cent, drawGun): a player's gun, the gun on screen when ps
        // isn't null, as CG_AddViewWeapon calls it after the frame's entities
        Sig("?? ?? ?? ?? ?? 1D ?? ?? ?? ?? 56 8B 74 24 34 57 85 F6 74 19 56 C7 44 24 10 01 00 00 00 E8", GAType::Code,
            0) > CG_AddPlayerWeapon;
        // CG_GetWeaponMaterialOverride(localClientNum, origin, variantDef, renderOptions, modelMask, shaderData, 0,
        // -1): the camo of a weapon's models in a DObj (the models of modelMask), for R_AddDObjToScene, as CG_Player
        // draws a player
        Sig("?? ?? ?? ?? ?? 4C 24 1C 8B 54 24 18 50 8B 44 24 18 51 8B 4C 24 18 52 8B 54 24 18 50 8B 44 24 18 51 8B",
            GAType::Code, 0) > CG_GetWeaponMaterialOverride;
        // the client's entities (handles 0x400 to 0x5FF), whose poses CG_GetPose and the effects bolted to them use:
        // the pose of handle h is at clientEntities + (localClientNum * 0x200 + h - 0x400) * 0x32C + 4
        Sig("7D 32 3D 00 04 00 00 8B 4C 24 04 7C 19 8B 15 ?? ?? ?? ?? C1 E1 09 03 C8 69 C9 2C 03 00 00",
            GAType::Data, 15, Lambda::DereferenceAddress) > clientEntities;
        // CG_PlayBoltedEffect(localClientNum, effect, handle, tagName), as the client script's playfxontag calls it
        Sig("?? ?? ?? ?? ?? 18 56 8B 74 24 18 57 8B 7C 24 14 C6 44 24 0F", GAType::Code, 0) > CG_PlayBoltedEffect;
        // CG_PlaySoundAlias(localClientNum, entnum, origin, 0, 0, volume, aliasHash) and the hash of an alias' name,
        // as the client script's playsound calls them
        Sig("6A 00 23 CA 51 68 FE 03 00 00 56 ?? ?? ?? ?? ?? 6A 01 50", GAType::Code, 11, Lambda::FollowCall)
            > CG_PlaySoundAlias;
        Sig("?? ?? ?? ?? ?? 85 ED 74 51 80 7D 00 00 74 4B 8B C5 8D 50 01", GAType::Code, 0) > SND_HashName;
        // FX_KillEffect(localClientNum, handle, immediately), as the client script's deletefx calls it; handles carry
        // a generation, so one of an effect that is gone does nothing
        Sig("?? ?? ?? ?? ?? 80 B8 C0 02 00 00 00 56 8B F0 74 21 8B 4C 24", GAType::Code, 0) > FX_KillEffect;
        // FX_SpawnEffect(system, def, ...): spawns an effect and returns its handle (0 when the 1024 effects are all in
        // use); every way of spawning one comes here, also the effects' own elements on the threads that update them
        Sig("?? ?? ?? ?? ?? 83 EC 14 80 78 18 00 56 8B 74 24 1C 74 10 80 BE C3 02 00 00 00 74 07", GAType::Code,
            0) > FX_SpawnEffect;
        // FX_EffectFromHandle(system, handle): the effect, or null when it is gone
        Sig("?? ?? ?? ?? ?? 82 54 03 00 00 85 C0 75 01 C3 56 8B 74 24 0C", GAType::Code, 0) > FX_EffectFromHandle;
        // FX_PlayOrientedEffect(localClientNum, def, msecBegin, origin, axis, markEntnum), as the client script's playfx
        // plays one (the helicopters' dust, aircraft_dustkick): FX_SpawnEffect, then the spawner's reference let go, so
        // it ends by itself (a looping one once its looping life is over) and is freed; returns its handle, 0 for none
        Sig("?? ?? ?? ?? ?? 80 78 18 00 53 55 8B 2D ?? ?? ?? ?? 56 75 04 33 DB EB 3D", GAType::Code, 0)
            > FX_PlayOrientedEffect;
        // The sun's shadow map size as the renderer makes its render targets (at its start and in a device reset): the
        // constant of "mov ecx, 0x400" (each half of the map is that square, the target twice as tall), stored in a
        // global every use of the map reads (512 instead on a low-memory path)
        Sig("83 3D ?? ?? ?? ?? 00 B9 ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 74 13 39 0D ?? ?? ?? ?? 77 0B B9 00 02 00 00",
            GAType::Data, 8) > sunShadowMapSizeConstant;
        Sig("83 3D ?? ?? ?? ?? 00 B9 ?? ?? ?? ?? 89 0D ?? ?? ?? ?? 74 13 39 0D ?? ?? ?? ?? 77 0B B9 00 02 00 00",
            GAType::Data, 14, Lambda::DereferenceAddress) > sunShadowMapSize;
        // FX_InitMarksSystem(marksSystem) empties the impact marks (decals) of a local client, as FX_InitSystem does
        // when a map loads; marksSystems points to one of 0x26028 bytes per local client
        Sig("?? ?? ?? ?? ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 11 E8 ?? ?? ?? ?? 8B", GAType::Code, 0) > FX_InitMarksSystem;
        Sig("83 C4 48 57 6A 00 50 A3 ?? ?? ?? ?? E8", GAType::Data, 8, Lambda::DereferenceAddress) > fx_marksSystems;
        // The client's handlers of the naked and night vision configstrings (0x60E and 0x60F, "\"<vision>\" <ms>", set by
        // the server's VisionSetNaked and VisionSetNight), (localClientNum): CG_ConfigStringModified calls them as one
        // changes, the client's start once with the map
        Sig("?? ?? ?? ?? ?? ?? ?? ?? 68 0E 06 00 00 E8 ?? ?? ?? ?? 89 44 24 08 8D 44 24 08 50 E8", GAType::Code, 0)
            > CG_VisionNakedConfigStringModified;
        Sig("?? ?? ?? ?? ?? ?? ?? ?? 68 0F 06 00 00 E8 ?? ?? ?? ?? 89 44 24 08 8D 44 24 08 50 E8", GAType::Code, 0)
            > CG_VisionNightConfigStringModified;
        // CG_GetImpactEffect(weaponIndex, surfaceType, flags, &effect, &sound): what a bullet of a weapon plays where it
        // hits a surface, from the map's impact table (CG_BulletHitEvent, 0x7FD4A0, asks it)
        Sig("?? ?? ?? ?? ?? 55 56 57 50 E8 ?? ?? ?? ?? 8B 6C 24 28 8B 5C 24 20 C7 45 00 00 00 00 00 8B 50 28",
            GAType::Code, 0) > CG_GetImpactEffect;
        // FX_PlayEffectOnBone(localClientNum, def, msecBegin, dobjHandle, bone, origin, axis): plays an effect at a point of
        // the world and keeps it there on the bone, as a bullet's blood follows a body (0x7FD4A0)
        Sig("?? ?? ?? ?? ?? 4C 24 18 8B 54 24 14 55 56 57 8B 3D ?? ?? ?? ?? 50 8B 44 24 20 51 8B 4C 24 20 52 8B 54 24 20 50",
            GAType::Code, 0) > FX_PlayEffectOnBone;
        // SND_AddLoopEmitter(aliasHash, origin) and SND_RemoveLoopEmitter(aliasHash, origin), the client script's
        // soundloopemitter and soundstoploopemitter: a looping alias at a point, which CG_PlaySoundAlias won't play
        Sig("?? ?? ?? ?? ?? 85 C0 74 32 E8 ?? ?? ?? ?? 85 C0 74 29 8B 4C 24 04 89 48 04 8B 4C 24 08 C7 00 16 00 00 00 D9",
            GAType::Code, 0) > SND_AddLoopEmitter;
        Sig("?? ?? ?? ?? ?? 85 C0 74 32 E8 ?? ?? ?? ?? 85 C0 74 29 8B 4C 24 04 89 48 04 8B 4C 24 08 C7 00 17 00 00 00 D9",
            GAType::Code, 0) > SND_RemoveLoopEmitter;
        // CG_SpawnTracer(localClientNum, start, end, 0, 0.0f, 0.0f), as the client script's bullettracer calls it
        Sig("?? ?? ?? ?? ?? 35 ?? ?? ?? ?? 83 BE 84 06 04 00 00 74 15 E8", GAType::Code, 0) > CG_SpawnTracer;

        // The Change Map menu (select_map; see MapMenu). UI_LoadMapList() fills the UI's map
        // list (128 entries of 0x130 bytes, the count before it) from mp/mapsTable.csv, as the UI starts
        Sig("?? ?? ?? ?? ?? 56 57 6A 12 8D 44 24 28 68 ?? ?? ?? ?? 50 E8", GAType::Code, 0) > UI_LoadMapList;
        Sig("68 00 98 00 00 33 ED 55 68 ?? ?? ?? ?? E8", GAType::Data, 9, Lambda::DereferenceAddress) > ui_mapList;
        // the map list's feeder: how many maps the list shows, the map list index of its row (0 for none), and that
        // entry's map name (+0x20 of the entry); each lists pack 0, or the packs owned while ui_showDLCMaps is set
        Sig("?? ?? ?? ?? ?? ?? 33 DB E8 ?? ?? ?? ?? 83 C4 04 84 C0 0F 84 9C 00 00 00", GAType::Code, 0)
            > UI_MapListCount;
        Sig("?? ?? ?? ?? ?? 0C 56 57 68 ?? ?? ?? ?? 33 DB E8", GAType::Code, 0) > UI_MapListIndex;
        Sig("?? ?? ?? ?? ?? ?? ?? ?? ?? 33 ED E8 ?? ?? ?? ?? 83 C4 04 84 C0 0F 84 B1 00 00 00", GAType::Code, 0)
            > UI_MapListEntryName;
        // Item_IsVisible(dc, localClientNum, item): its window's visible flag, show and hide bits and 'visible when'
        Sig("?? ?? ?? ?? ?? 8B 74 24 10 8B 4C 86 54 C1 E9 02", GAType::Code, 0) > Item_IsVisible;
        // DB_FindXAssetHeader(type, name), the default asset for a name not registered (Plutonium hooks its start)
        Sig("?? ?? ?? ?? ?? 89 04 24 89 44 24 04 B8 ?? ?? ?? ?? 8D 50 01 8A 08", GAType::Code, 0) > DB_FindXAssetHeader;
        // the UI's context of the local client, whose menus (pairs of name hash and menuDef_t*, sorted, from +0x23C,
        // the count at +0x14FC) the menu commands find a menu in
        Sig("8B 44 24 04 50 E8 ?? ?? ?? ?? 83 C4 04 B8 ?? ?? ?? ?? C3 CC", GAType::Data, 14,
            Lambda::DereferenceAddress) > uiContext;
        // UI_StartServer(localClientNum), the LAN setup's START MATCH (uiScript StartServer): "wait ; wait ; map <the
        // map list's entry at ui_currentNetMap>" with the game mode
        Sig("?? ?? ?? ?? ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 48 18 83 C4 04 6A 00 6A 01 51 68", GAType::Code, 0)
            > UI_StartServer;

        // Live actors. SV_AddTestClient() adds a test client as the script's addtestclient
        // does and returns its entity; SV_BotUserMove(client, cmd) works out what a test client does this server frame
        Sig("?? ?? ?? ?? ?? F0 81 EC 84 04 00 00 53 56 57 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 85 C0 75 16 E8",
            GAType::Code, 0) > SV_AddTestClient;
        Sig("?? ?? ?? ?? ?? 74 24 14 8B CE 2B 0D ?? ?? ?? ?? 8B 5C 24 18 B8 6D 8E EF 83", GAType::Code,
            0) > SV_BotUserMove;
        // SV_BotsThink(), which moves each test client, on the server's thread before the scripts' frame
        Sig("?? ?? ?? ?? ?? C7 44 24 0C 00 00 00 00 C7 44 24 10 00 00 00 00 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 8B 35 ?? ?? "
            "?? ?? "
            "33 FF 39 78 18",
            GAType::Code, 0) > SV_BotsThink;
        // the server's clients and time, and its entities with their size (see the end of SV_AddTestClient)
        Sig("8B 74 24 14 8B CE 2B 0D ?? ?? ?? ?? 8B 5C 24 18 B8 6D 8E EF 83", GAType::Data, 8,
            Lambda::DereferenceAddress) > svs_clients;
        Sig("8B 0D ?? ?? ?? ?? 53 57 55 89 0B C6 43 1E 00 C6 43 1F 00", GAType::Data, 2,
            Lambda::DereferenceAddress) > svs_time;
        Sig("A1 ?? ?? ?? ?? 0F AF C7 83 C4 2C 03 05 ?? ?? ?? ?? 5F 5E 5B 8B E5 5D C3", GAType::Data, 1,
            Lambda::DereferenceAddress) > sv_gentitySize;
        Sig("A1 ?? ?? ?? ?? 0F AF C7 83 C4 2C 03 05 ?? ?? ?? ?? 5F 5E 5B 8B E5 5D C3", GAType::Data, 13,
            Lambda::DereferenceAddress) > sv_gentities;
        // the script machine's parameters, as the game's code calls scripts and scripts call the game:
        // Scr_AddString(text, instance), Scr_AddInt(value, instance), Scr_AddVector(vector, instance),
        // Scr_Notify(entity, name, parameters), Scr_ClearOutParams(instance), and the count of the parameters a
        // function being called gets
        Sig("?? ?? ?? ?? ?? 10 57 E8 ?? ?? ?? ?? 8B F7 69 F6 20 43 00 00 8B 86 ?? ?? ?? ?? 83 C4 04 3B 86 ?? ?? ?? ?? "
            "75 "
            "0D 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 83 86 ?? ?? ?? ?? 08 8B 4C 24 0C 8B 86 ?? ?? ?? ?? FF 86 ?? ?? "
            "?? "
            "?? 57 6A 00 51 C7 40 04 02 00 00 00 E8",
            GAType::Code, 0) > Scr_AddString;
        Sig("?? ?? ?? ?? ?? 56 E8 ?? ?? ?? ?? 69 F6 20 43 00 00 8B 86 ?? ?? ?? ?? 83 C4 04 3B 86 ?? ?? ?? ?? 75 0D 68 "
            "?? "
            "?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 83 86 ?? ?? ?? ?? 08 8B 86 ?? ?? ?? ?? FF 86 ?? ?? ?? ?? 8B 54 24 08 C7 "
            "40 "
            "04 06",
            GAType::Code, 0) > Scr_AddInt;
        Sig("?? ?? ?? ?? ?? 10 57 E8 ?? ?? ?? ?? 8B F7 69 F6 20 43 00 00 8B 86 ?? ?? ?? ?? 83 C4 04 3B 86 ?? ?? ?? ?? "
            "75 "
            "0D 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 8B 4C 24 0C 83 86",
            GAType::Code, 0) > Scr_AddVector;
        Sig("?? ?? ?? ?? ?? B7 4C 24 08 8B 54 24 04 50 8B 02 51 6A 00 50 E8 ?? ?? ?? ?? 83 C4 10 C3", GAType::Code,
            0) > Scr_Notify;
        Sig("?? ?? ?? ?? ?? 0C 8B F7 69 F6 20 43 00 00 83 BE ?? ?? ?? ?? 00 74 74", GAType::Code,
            0) > Scr_ClearOutParams;
        Sig("8B F7 69 F6 20 43 00 00 83 BE ?? ?? ?? ?? 00 74 74", GAType::Data, 10,
            Lambda::DereferenceAddress) > scr_outparamcount;
        // the game's table of what scripts can call on a player (name, function, developer only), 166 of them
        Sig("8B 8E ?? ?? ?? ?? 8B C3 8A 10 3A 11 75 1A 84 D2 74 12 8A 50 01 3A 51 01 75 0E 83 C0 02 83 C1 02 84 D2 75 "
            "E4 "
            "33 C0 EB 05 1B C0 83 D8 FF 85 C0 74 13 83 C6 0C 47 81 FE C8 07 00 00",
            GAType::Data, 2, Lambda::DereferenceAddress) > player_methods;
        // What the player of a live game can ask of it. The scripts' load, where the game
        // looks up its callbacks: GScr_LoadGameTypeScript(), Scr_LoadScript(instance, name),
        // Scr_GetFunctionHandle(instance, name, function); and calling one: Scr_ExecEntThread(entity, function,
        // parameters), Scr_FreeThread(thread, instance)
        Sig("?? ?? ?? ?? ?? 8B 48 18 83 EC 40 56 51 68", GAType::Code, 0) > GScr_LoadGameTypeScript;
        Sig("?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 8C 24 08 38 00 00", GAType::Code, 0) > Scr_LoadScript;
        Sig("?? ?? ?? ?? ?? EC 08 53 55 56 8B 74 24 18 57 50", GAType::Code, 0) > Scr_GetFunctionHandle;
        Sig("?? ?? ?? ?? ?? 4C 24 08 8B 54 24 04 6A 00 50 8B 02", GAType::Code, 0) > Scr_ExecEntThread;
        Sig("?? ?? ?? ?? ?? 8B 4C 24 08 50 51 E8 ?? ?? ?? ?? 83 C4 08 C3 CC CC", GAType::Code, 0) > Scr_FreeThread;
        // G_FreeEntity(entity), and G_Damage(target, inflictor, attacker, direction, point, damage, flags, means of
        // death, weapon, hit location, bone, model, time offset)
        Sig("?? ?? ?? ?? ?? 57 56 E8 ?? ?? ?? ?? 83 C4 04 83 BE 84 02 00 00 00", GAType::Code, 0) > G_FreeEntity;
        Sig("?? ?? ?? ?? ?? 74 24 14 83 BE 44 01 00 00 00 74 ?? 8B 44 24 44 8B 4C 24 38", GAType::Code, 0) > G_Damage;
        // A shot: Bullet_Fire(attacker, spread, weapon, weapon entity, game time), the line a pellet takes,
        // Bullet_Endpos(seed, spread, end, direction, weapon, range), called from it (it has a twin by its bytes), and
        // G_LocationalTrace(results, start, end, ignored entity, mask, hit priorities) with a rifle's and a pistol's
        // priorities, as Bullet_Trace chooses them
        Sig("?? ?? ?? ?? ?? 00 53 55 8B AC 24 84 03 00 00 56", GAType::Code, 0) > Bullet_Fire;
        Sig("8D 0C 2B F3 0F 11 44 24 4C F3 0F 10 46 2C 51 F3 0F 11 44 24 54 ?? ?? ?? ?? ?? 8B 15 ?? ?? ?? ?? 83 C4 18",
            GAType::Code, 21, Lambda::FollowCall) > Bullet_Endpos;
        Sig("?? ?? ?? ?? ?? 74 24 48 8D 44 24 04 56 50 E8 ?? ?? ?? ?? 8B 4C 24 54 83 C4 08 51 8D 4C 24 14 C6 44 24 13 00",
            GAType::Code, 0) > G_LocationalTrace;
        Sig("80 BF 4D 05 00 00 00 B8 ?? ?? ?? ?? 75 05 B8 ?? ?? ?? ?? 8B 4B 04", GAType::Data, 8,
            Lambda::DereferenceAddress) > bulletPriorityRifle;
        Sig("80 BF 4D 05 00 00 00 B8 ?? ?? ?? ?? 75 05 B8 ?? ?? ?? ?? 8B 4B 04", GAType::Data, 15,
            Lambda::DereferenceAddress) > bulletPriorityPistol;
        // set while the server's give runs: the weapon look-up then registers a weapon the map didn't precache
        Sig("83 3D ?? ?? ?? ?? 00 74 13 8B 44 24 04 68 ?? ?? ?? ?? 50 E8", GAType::Data, 2,
            Lambda::DereferenceAddress) > g_registerAnyWeapon;

#undef Sig
#undef Lambda
    };
}  // namespace IWXMVM::T5::Signatures
