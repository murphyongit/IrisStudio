#include "StdInclude.hpp"
#include "Sky.hpp"

#include "Addresses.hpp"
#include "ContentPacksInternal.hpp"
#include "Structures.hpp"

#include <optional>
#include <unordered_set>

namespace IWXMVM::T5::Sky
{
    using namespace Structures;
    using namespace ContentPacks;

    namespace
    {
        bool DrawsSky(std::string_view techniqueSet)
        {
            // (mc_sky_cubemap_hdr, mc_sky_cubemap_hdr_cl1, wc_sky: "sky" as a word of the name, not "skin")
            std::string lower(techniqueSet);
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (std::size_t at = lower.find("sky"); at != std::string::npos; at = lower.find("sky", at + 1))
            {
                const bool startsWord = at == 0 || lower[at - 1] == '_';
                const bool endsWord = at + 3 == lower.size() || lower[at + 3] == '_';
                if (startsWord && endsWord)
                    return true;
            }
            return false;
        }
    }  // namespace

    std::vector<Types::SkyImage> GetSkyImages()
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

        std::vector<Types::SkyImage> images;
        std::unordered_set<const GfxImage*> seen;
        forEachEntry([&](const XAssetEntry& entry) {
            if (entry.type != ASSET_TYPE_MATERIAL || entry.zoneIndex != *mapZone)
                return;
            const auto material = static_cast<const Material*>(entry.header);
            if (!material->techniqueSet || !material->techniqueSet->name || !material->textureTable ||
                !DrawsSky(material->techniqueSet->name))
                return;
            for (std::uint8_t i = 0; i < material->textureCount; ++i)
            {
                const auto image = material->textureTable[i].image;
                if (!image || !seen.insert(image).second)
                    continue;
                images.push_back({reinterpret_cast<IDirect3DBaseTexture9* const*>(&image->texture),
                                  image->name ? image->name : "", material->name ? material->name : "",
                                  material->techniqueSet->name});
            }
        });
        return images;
    }
}  // namespace IWXMVM::T5::Sky
