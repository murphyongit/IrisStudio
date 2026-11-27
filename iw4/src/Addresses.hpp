#pragma once
#include "StdInclude.hpp"

// Modern Warfare 2 multiplayer, build 177 (the iw4mp.exe that IW4M and IW4x both start; its code is the same in
// iw4m.exe and iw4x.exe, checked byte for byte). One build only, so the addresses are written down, not searched
// for. Where an address came from: IW4x's source (github.com/iw4x/iw4x-client) or the disassembly, see NOTES.md.
namespace IWXMVM::IW4::Addresses
{
    // functions
    inline constexpr std::uintptr_t Cbuf_AddText = 0x404B20;             // (int localClientNum, const char* text)
    inline constexpr std::uintptr_t Dvar_FindVar = 0x4D5390;             // (const char* name)
    inline constexpr std::uintptr_t Dvar_SetStringByName = 0x44F060;     // (const char* name, const char* value)
    inline constexpr std::uintptr_t Com_TimeScaleMsec = 0x60B380;        // int (int msec)
    inline constexpr std::uintptr_t CL_PlayDemo_f = 0x5A86D0;            // the "demo" command
    inline constexpr std::uintptr_t CL_DemoRead = 0x5A8660;              // int (void* buffer, int len, int file)
    inline constexpr std::uintptr_t CL_FirstSnapshot = 0x5A1CD0;         // localClientNum in esi
    inline constexpr std::uintptr_t CL_FirstSnapshot_TimeJumped = 0x5A1D30;  // its call of Con_TimeJumped
    inline constexpr std::uintptr_t CG_ExecuteNewServerCommands = 0x4BE6E0;  // (int localClientNum, int latest)
    inline constexpr std::uintptr_t CG_DrawActiveFrame_AfterView = 0x486D2B; // call of 0x598AA0, after CG_CalcViewValues
    inline constexpr std::uintptr_t CG_AfterCalcViewValues = 0x598AA0;
    inline constexpr std::uintptr_t Com_GetClientDObj = 0x41FF50;        // (int handle, int localClientNum)
    inline constexpr std::uintptr_t CG_DObjGetWorldTagMatrix = 0x426660; // (pose, obj, tagName, axis[3][3], origin)
    inline constexpr std::uintptr_t CG_DObjGetWorldTagPos = 0x426580;    // (pose, obj, tagName, pos)
    inline constexpr std::uintptr_t SL_FindString = 0x434EE0;            // (const char* text)
    inline constexpr std::uintptr_t SL_GetString = 0x4CDC10;             // (const char* text, unsigned user)
    inline constexpr std::uintptr_t IN_Frame = 0x475E10;
    inline constexpr std::uintptr_t MainWndProc = 0x4731F0;
    inline constexpr std::uintptr_t R_AddCmdDrawText = 0x509D80;

    // data
    inline constexpr std::uintptr_t cg = 0x7F0F78;
    inline constexpr std::uintptr_t cgs = 0x7ED3B8;
    inline constexpr std::uintptr_t cg_entities = 0x8F3CA8;
    inline constexpr std::uintptr_t clientInfo = 0x8E77B0;               // cg.bgs.clientinfo, 18 of clientInfo_t
    inline constexpr std::uintptr_t clientActive = 0xB2C698;
    inline constexpr std::uintptr_t clientConnection = 0xA1E878;
    inline constexpr std::uintptr_t clientStatic = 0xA7FE90;
    inline constexpr std::uintptr_t clientUIActive = 0xB2BB88;
    inline constexpr std::uintptr_t con = 0x9FCCF8;
    inline constexpr std::uintptr_t d3d9Device = 0x66DEF88;              // IDirect3DDevice9*
    inline constexpr std::uintptr_t fs_searchpaths = 0x63D96E0;          // searchpath_s*
}  // namespace IWXMVM::IW4::Addresses
