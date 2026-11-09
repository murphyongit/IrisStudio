#include "StdInclude.hpp"
#include "Surfaces.hpp"

#include "Addresses.hpp"
#include "ContentPacksInternal.hpp"
#include "Structures.hpp"
#include "Components/SurfaceLogic.hpp"

namespace IWXMVM::T5::Surfaces
{
    using namespace Structures;
    using namespace ContentPacks;

    std::vector<Types::NormalMap> GetMapNormalMaps()
    {
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        const auto forEachEntry = [&](auto function) {
            for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
                for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
                    if (entries[index].header)
                        function(entries[index]);
        };

        // the map's zone: the one of its world (a campaign level's too)
        std::optional<std::uint8_t> mapZone;
        forEachEntry([&](const XAssetEntry& entry) {
            if (entry.type == ASSET_TYPE_GFXWORLD)
                mapZone = entry.zoneIndex;
        });
        if (!mapZone)
            return {};

        // its materials' normal maps, those of its characters left out (H1: the map and its props), each with what its
        // relief counts for in the materials that take depth
        std::vector<Types::NormalMap> normalMaps;
        std::unordered_map<const void*, std::size_t> seen;
        forEachEntry([&](const XAssetEntry& entry) {
            if (entry.type != ASSET_TYPE_MATERIAL || entry.zoneIndex != *mapZone)
                return;
            const auto material = static_cast<const Material*>(entry.header);
            if (!material->techniqueSet || !material->textureTable ||
                !Components::SurfaceLogic::IsMapSurface(material->techniqueSet->name))
                return;
            const auto weight =
                Components::SurfaceLogic::TakesDepth(material->techniqueSet->name, material->layeredSurfaceTypes)
                    ? Components::SurfaceLogic::GetReliefWeight(material->layeredSurfaceTypes)
                    : 0.0f;
            for (std::uint8_t i = 0; i < material->textureCount; ++i)
            {
                const auto& texture = material->textureTable[i];
                if (texture.semantic != TS_NORMAL_MAP || !texture.image)
                    continue;
                const auto [found, first] = seen.try_emplace(texture.image, normalMaps.size());
                if (first)
                    normalMaps.push_back({reinterpret_cast<IDirect3DBaseTexture9* const*>(&texture.image->texture),
                                          texture.image->name, 0.0f});
                // (a blend's upper layers are another surface's: snow over a wall's concrete)
                if (texture.nameEnd == 'p')
                    normalMaps[found->second].reliefWeight = std::max(normalMaps[found->second].reliefWeight, weight);
            }
        });
        return normalMaps;
    }
}  // namespace IWXMVM::T5::Surfaces
