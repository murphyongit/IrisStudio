#pragma once
#include "StdInclude.hpp"

#include "Structures.hpp"

namespace IWXMVM::IW4::Functions
{
    Structures::dvar_t* FindDvar(const std::string_view name);
    // The same, quietly: nullptr when the game has no such setting (FindDvar logs it once).
    Structures::dvar_t* TryFindDvar(const char* name);

    void SetBool(const char* name, bool value);
    void SetFloat(const char* name, float value);
    void SetInt(const char* name, int value);
    bool GetBool(const char* name, bool fallback = false);
    float GetFloat(const char* name, float fallback = 0.0f);

    void Cbuf_AddText(std::string command);
    void Dvar_SetStringByName(const char* dvarName, const char* value);

    // Where a demo by this name (with or without the extension) is, among the game's folders. "" when nowhere.
    std::string GetFilePath(const std::string_view demoName);
    // The first folder the game reads loose files from (the one "demos/..." is looked up in first).
    std::filesystem::path GetFirstSearchDirectory();

    Structures::DObj* Com_GetClientDObj(int handle);
    unsigned int SL_FindString(const char* text);
    bool CG_DObjGetWorldTagMatrix(const Structures::cpose_t* pose, Structures::DObj* obj, unsigned int tagName,
                                  float (*axis)[3], float* origin);
    void CG_ExecuteNewServerCommands(int latestSequence);
}  // namespace IWXMVM::IW4::Functions
