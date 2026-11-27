#include "StdInclude.hpp"
#include "Sky.hpp"

#include "Addresses.hpp"
#include "Structures.hpp"

namespace IWXMVM::IW4::Sky
{
    namespace
    {
        void __cdecl TakeWorld(Structures::XAssetHeader header, void* data)
        {
            *static_cast<Structures::GfxWorld**>(data) = header.gfxWorld;
        }
    }  // namespace

    std::vector<Types::SkyImage> GetSkyImages()
    {
        typedef void(__cdecl * DB_EnumXAssets_t)(Structures::XAssetType type,
                                                 void(__cdecl * function)(Structures::XAssetHeader, void*),
                                                 void* data, bool overrides);

        Structures::GfxWorld* world = nullptr;
        reinterpret_cast<DB_EnumXAssets_t>(Addresses::DB_EnumXAssets)(Structures::ASSET_TYPE_GFXWORLD, TakeWorld,
                                                                      &world, false);
        if (!world || !world->skies || world->skyCount <= 0 || world->skyCount > 16)
            return {};

        std::vector<Types::SkyImage> images;
        for (int i = 0; i < world->skyCount; ++i)
        {
            const auto image = world->skies[i].skyImage;
            if (!image)
                continue;

            images.push_back({reinterpret_cast<IDirect3DBaseTexture9* const*>(&image->texture),
                              image->name ? image->name : "", world->name ? world->name : "", "sky"});
        }
        return images;
    }
}  // namespace IWXMVM::IW4::Sky
