#include "StdInclude.hpp"
#include "Functions.hpp"

#include "Addresses.hpp"

#include <mutex>
#include <set>

namespace IWXMVM::IW4::Functions
{
    Structures::dvar_t* TryFindDvar(const char* name)
    {
        typedef Structures::dvar_t*(__cdecl * Dvar_FindVar_t)(const char* name);
        return reinterpret_cast<Dvar_FindVar_t>(Addresses::Dvar_FindVar)(name);
    }

    Structures::dvar_t* FindDvar(const std::string_view name)
    {
        const std::string text{name};
        const auto dvar = TryFindDvar(text.c_str());
        if (!dvar)
        {
            // said once for each: these are asked for every frame
            static std::mutex mutex;
            static std::set<std::string> missing;
            const std::lock_guard lock{mutex};
            if (missing.insert(text).second)
                LOG_WARN("The game has no setting named {}", text);
        }
        return dvar;
    }

    void SetBool(const char* name, bool value)
    {
        if (const auto dvar = FindDvar(name); dvar && dvar->current.enabled != value)
            dvar->current.enabled = value;
    }

    void SetFloat(const char* name, float value)
    {
        if (const auto dvar = FindDvar(name); dvar && dvar->current.value != value)
            dvar->current.value = value;
    }

    void SetInt(const char* name, int value)
    {
        if (const auto dvar = FindDvar(name); dvar && dvar->current.integer != value)
            dvar->current.integer = value;
    }

    bool GetBool(const char* name, bool fallback)
    {
        const auto dvar = FindDvar(name);
        return dvar ? dvar->current.enabled : fallback;
    }

    float GetFloat(const char* name, float fallback)
    {
        const auto dvar = FindDvar(name);
        return dvar ? dvar->current.value : fallback;
    }

    void Cbuf_AddText(std::string command)
    {
        LOG_DEBUG("Executing command \"{0}\"", command);
        command.append("\n");

        typedef void(__cdecl * Cbuf_AddText_t)(int localClientNum, const char* text);
        reinterpret_cast<Cbuf_AddText_t>(Addresses::Cbuf_AddText)(0, command.c_str());
    }

    void Dvar_SetStringByName(const char* dvarName, const char* value)
    {
        typedef void(__cdecl * Dvar_SetStringByName_t)(const char* dvarName, const char* value);
        reinterpret_cast<Dvar_SetStringByName_t>(Addresses::Dvar_SetStringByName)(dvarName, value);
    }

    namespace
    {
        template <typename F>
        void ForEachSearchDirectory(F&& visit)
        {
            auto searchpath = *reinterpret_cast<Structures::searchpath_s**>(Addresses::fs_searchpaths);
            // (a list that goes round, or on for ever, would be a wrong address: stop well before)
            for (int i = 0; searchpath && i < 512; searchpath = searchpath->next, ++i)
            {
                if (!searchpath->dir)
                    continue;

                auto path = std::filesystem::path(searchpath->dir->path);
                path.append(searchpath->dir->gamedir);
                if (visit(path))
                    return;
            }
        }
    }  // namespace

    std::string GetFilePath(const std::string_view demoName)
    {
        std::string file{demoName};
        if (!file.ends_with(".dm_13"))
            file += ".dm_13";

        std::string found;
        ForEachSearchDirectory([&](const std::filesystem::path& directory) {
            std::error_code error;
            const auto path = directory / "demos" / file;
            if (std::filesystem::exists(path, error))
            {
                found = path.string();
                return true;
            }
            return false;
        });
        return found;
    }

    std::filesystem::path GetFirstSearchDirectory()
    {
        std::filesystem::path first;
        ForEachSearchDirectory([&](const std::filesystem::path& directory) {
            first = directory;
            return true;
        });
        return first;
    }

    Structures::DObj* Com_GetClientDObj(int handle)
    {
        typedef Structures::DObj*(__cdecl * Com_GetClientDObj_t)(int handle, int localClientNum);
        return reinterpret_cast<Com_GetClientDObj_t>(Addresses::Com_GetClientDObj)(handle, 0);
    }

    unsigned int SL_FindString(const char* text)
    {
        typedef unsigned int(__cdecl * SL_FindString_t)(const char* text);
        return reinterpret_cast<SL_FindString_t>(Addresses::SL_FindString)(text);
    }

    bool CG_DObjGetWorldTagMatrix(const Structures::cpose_t* pose, Structures::DObj* obj, unsigned int tagName,
                                  float (*axis)[3], float* origin)
    {
        typedef int(__cdecl * CG_DObjGetWorldTagMatrix_t)(const Structures::cpose_t* pose, Structures::DObj* obj,
                                                         unsigned int tagName, float (*axis)[3], float* origin);
        return reinterpret_cast<CG_DObjGetWorldTagMatrix_t>(Addresses::CG_DObjGetWorldTagMatrix)(pose, obj, tagName,
                                                                                                axis, origin) != 0;
    }

    void CG_ExecuteNewServerCommands(int latestSequence)
    {
        typedef void(__cdecl * CG_ExecuteNewServerCommands_t)(int localClientNum, int latestSequence);
        reinterpret_cast<CG_ExecuteNewServerCommands_t>(Addresses::CG_ExecuteNewServerCommands)(0, latestSequence);
    }
}  // namespace IWXMVM::IW4::Functions
