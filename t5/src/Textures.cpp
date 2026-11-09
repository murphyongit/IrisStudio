#include "StdInclude.hpp"
#include "Textures.hpp"

#include "Addresses.hpp"
#include "ContentPacks.hpp"
#include "Functions.hpp"
#include "ContentPacksInternal.hpp"
#include "Structures.hpp"
#include "Components/Actors.hpp"
#include "Components/CharacterLogic.hpp"

namespace IWXMVM::T5::Textures
{
    using namespace Structures;
    using namespace ContentPacks;
    using Components::TextureLogic::Group;

    namespace
    {
        std::string Lower(std::string_view text)
        {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower;
        }

        // the models weapons are drawn with: each variant's view models, hands and world models
        void AddWeaponModels(std::unordered_set<const XModel*>& models)
        {
            const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
            constexpr std::size_t MODELS = 16;
            for (std::size_t index = 1; index < Functions::GetWeaponVariantCount(); ++index)
            {
                const auto def = variants[index]->weapDef;
                if (!def)
                    continue;
                // (the hands at 0x08, OpenAssetTools' T5 WeaponDef::handXModel)
                if (const auto hands = *reinterpret_cast<XModel* const*>(reinterpret_cast<const std::uint8_t*>(def) + 8))
                    models.insert(hands);
                for (const auto list : {def->gunXModel, def->worldModel})
                    if (list)
                        for (std::size_t i = 0; i < MODELS; ++i)
                            if (list[i])
                                models.insert(list[i]);
            }
        }

        void AddNames(std::unordered_set<std::string>& names, const Types::Character& character)
        {
            for (const auto* list : {&character.bodies, &character.heads, &character.hats, &character.gears,
                                     &character.woundedTorsos, &character.woundedLegs})
                for (const auto& model : *list)
                    names.insert(Lower(model));
            for (const auto& model : character.GetModels())
                if (!model.empty())
                    names.insert(Lower(model));
        }
    }  // namespace

    Types::PackImages GetPackImages()
    {
        Types::PackImages result;
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        const auto forEachEntry = [&](auto function) {
            for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
                for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
                    if (entries[index].header)
                        function(entries[index]);
        };

        // the zones loaded, and the map's: the one of its world
        for (const auto& zone : GetLoadedZones() | std::views::keys)
            result.zones.push_back(Lower(zone));
        std::unordered_set<std::string> characterZoneNames;
        for (const auto& zone : GetCharacterZones())
            characterZoneNames.insert(Lower(zone.name));
        std::optional<std::uint8_t> mapZone, gunsZone;
        std::set<std::uint8_t> characterZones;
        std::vector<std::string> modelNames;
        forEachEntry([&](const XAssetEntry& entry) {
            if (entry.type == ASSET_TYPE_GFXWORLD)
                mapZone = entry.zoneIndex;
            const auto zone = Lower(GetZoneName(entry.zoneIndex));
            if (zone == Components::TextureLogic::GUNS_ZONE)
                gunsZone = entry.zoneIndex;
            if (characterZoneNames.contains(zone))
                characterZones.insert(entry.zoneIndex);
            if (entry.type == ASSET_TYPE_XMODEL)
                if (const auto model = static_cast<const XModel*>(entry.header); model->name)
                    modelNames.emplace_back(model->name);
        });
        if (mapZone)
            result.map = Lower(GetZoneName(*mapZone));

        // what the models are: the characters' (the map's own made of its models, with its dogs and killstreaks, and
        // the campaign's), the weapons'
        std::unordered_set<std::string> playerNames, weaponNames;
        for (const auto& character : Components::CharacterLogic::MakeMapCharacters(modelNames))
        {
            const bool weapon = character.kind == Types::Character::Kind::Dog ||
                                character.kind == Types::Character::Kind::Killstreak;
            AddNames(weapon ? weaponNames : playerNames, character);
        }
        for (const auto& name : Components::Actors::GetCampaignModelKinds() | std::views::keys)
            playerNames.insert(Lower(name));
        std::unordered_set<const XModel*> weaponModels;
        AddWeaponModels(weaponModels);

        // each image: weapons or players when a model of them uses it (through its materials), or a material of the
        // guns' zone does (a camo); the map's otherwise
        std::unordered_map<const GfxImage*, std::pair<bool, bool>> uses;
        const auto markMaterial = [&](const Material* material, bool weapon, bool player) {
            if (!material || !material->textureTable)
                return;
            for (std::uint8_t i = 0; i < material->textureCount; ++i)
                if (const auto image = material->textureTable[i].image)
                {
                    auto& use = uses[image];
                    use.first |= weapon;
                    use.second |= player;
                }
        };
        forEachEntry([&](const XAssetEntry& entry) {
            if (entry.type == ASSET_TYPE_XMODEL)
            {
                const auto model = static_cast<const XModel*>(entry.header);
                if (!model->name || !model->materialHandles)
                    return;
                const auto name = Lower(model->name);
                const bool weapon = weaponModels.contains(model) || weaponNames.contains(name);
                const bool player =
                    !weapon && (playerNames.contains(name) || characterZones.contains(entry.zoneIndex));
                if (!weapon && !player)
                    return;
                for (std::uint8_t surface = 0; surface < model->numsurfs; ++surface)
                    markMaterial(model->materialHandles[surface], weapon, player);
            }
            else if (entry.type == ASSET_TYPE_MATERIAL && gunsZone && entry.zoneIndex == *gunsZone)
                markMaterial(static_cast<const Material*>(entry.header), true, false);
        });

        forEachEntry([&](const XAssetEntry& entry) {
            if (entry.type != ASSET_TYPE_IMAGE)
                return;
            const auto image = static_cast<const GfxImage*>(entry.header);
            if (!image->name)
                return;
            const auto use = uses.find(image);
            const auto group = use == uses.end()
                                   ? Group::Maps
                                   : Components::TextureLogic::GetGroup(use->second.first, use->second.second);
            result.images.push_back(
                {Lower(image->name), reinterpret_cast<IDirect3DBaseTexture9* const*>(&image->texture), group});
        });
        return result;
    }
}  // namespace IWXMVM::T5::Textures
