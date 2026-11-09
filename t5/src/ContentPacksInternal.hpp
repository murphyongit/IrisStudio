#pragma once

#include <unordered_set>

#include "ContentPacks.hpp"
#include "Structures.hpp"

// What CampaignMaps shares of ContentPacks.cpp: registering a zone's assets, leaving them out (a copy of their header,
// whose resources are released after the load), and the render thread's releases
namespace IWXMVM::T5::ContentPacks
{
    inline constexpr std::int32_t ASSET_TYPE_GFXWORLD = 17;

    // what the load going on did (null when none counts)
    extern LoadResult* result;
    // the copies of what was left out (see KeepHeader), and those whose resources are released after the load
    extern std::vector<std::unique_ptr<std::uint8_t[]>> headerCopies;
    extern std::unordered_set<const void*> copies;
    extern std::vector<std::pair<std::int32_t, void*>> unloadableCopies;
    // the technique sets the pack registered
    extern std::unordered_set<const Structures::MaterialTechniqueSet*> packTechniqueSets;

    const char* GetAssetName(std::int32_t type, void* header);
    // the name of a loaded zone (an asset entry's zoneIndex)
    std::string_view GetZoneName(std::uint32_t zoneIndex);
    bool IsZoneLoaded(std::string_view name);
    // where the game finds a zone of that name, nothing when it has none
    std::optional<std::filesystem::path> FindZoneFile(const std::string& zone);
    // the registered asset of a type and name (not a stand-in), or null
    const Structures::XAssetEntry* FindRegisteredEntry(std::int32_t type, const char* name);
    void* FindRegistered(std::int32_t type, const char* name);
    bool PoolHasRoom(std::int32_t type);
    bool HasResources(std::int32_t type);
    std::unique_ptr<std::uint8_t[]> CopyHeader(std::int32_t type, const void* header);
    // leaves an asset out: a copy of its header, which what refers to it points to
    void* KeepHeader(std::int32_t type, void* header);
    // registers an asset (the game's DB_AddXAsset)
    void* AddXAsset(std::int32_t assetType, void* assetHeader);
    // registers what a kept model or material draws with that was left out before (see RegisterCopy)
    void RegisterMaterials(Structures::XModel& model);
    void RegisterTextures(Structures::Material& material);
    // takes the sound banks and patches left out out of the sound system's lists
    void RemoveSoundCopies();
    void ReleaseOnRenderThread(void* object);
    void ReleaseModelBuffersOnRenderThread(Structures::XModel& model);

    template <typename Function>
    void ForEachPass(const Structures::MaterialTechniqueSet& techniqueSet, Function function)
    {
        for (const auto technique : techniqueSet.techniques)
            for (std::uint16_t pass = 0; technique && pass < technique->passCount; ++pass)
                function(technique->passArray[pass]);
    }
}  // namespace IWXMVM::T5::ContentPacks
