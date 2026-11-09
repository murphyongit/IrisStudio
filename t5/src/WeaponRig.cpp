#include "StdInclude.hpp"
#include "WeaponRig.hpp"

#include "Addresses.hpp"
#include "AnimationPose.hpp"
#include "ContentPacks.hpp"
#include "Functions.hpp"
#include "Components/WeaponCatalog.hpp"

namespace IWXMVM::T5::WeaponRig
{
    using namespace Structures;

    const WeaponVariantDef* FindVariant(std::string_view name)
    {
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        for (std::size_t index = 1; index < Functions::GetWeaponVariantCount(); ++index)
            if (variants[index]->szInternalName && variants[index]->szInternalName == name)
                return variants[index];
        // a weapon of the weapon catalog, from its zone (only the game's weapons named in its catalog are looked for:
        // looking up a name the asset table hasn't is quick, but the table changes while a zone loads)
        if (Components::WeaponCatalog::Get().Has(name) && !ContentPacks::IsLoading())
            return static_cast<const WeaponVariantDef*>(
                ContentPacks::FindAsset(ASSET_TYPE_WEAPON, std::string(name).c_str()));
        return nullptr;
    }

    const WeaponVariantDef* FindLeftHandVariant(const WeaponVariantDef& variant)
    {
        const auto def = variant.weapDef;
        if (!def || !def->bDualWield)
            return nullptr;
        // a weapon of a zone the game hasn't registered has no index of its left one (what its def has there is
        // stale): the weapon catalog names it
        if (variant.szInternalName)
            if (const auto entry = Components::WeaponCatalog::Get().FindVariant(variant.szInternalName))
                return entry->left.empty() ? nullptr : FindVariant(entry->left);
        if (def->dualWieldWeaponIndex == 0 || def->dualWieldWeaponIndex >= MAX_WEAPON_VARIANTS)
            return nullptr;
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        return variants[def->dualWieldWeaponIndex];
    }

    const char* GetShotSound(const WeaponVariantDef& variant)
    {
        if (variant.szInternalName)
            if (const auto entry = Components::WeaponCatalog::Get().FindVariant(variant.szInternalName))
                return entry->sound.c_str();
        return variant.weapDef ? variant.weapDef->fireSound : nullptr;
    }

    bool FiresProjectiles(const WeaponVariantDef& variant)
    {
        return variant.weapDef && variant.weapDef->weapType == WEAPTYPE_PROJECTILE;
    }

    bool FiresRockets(const WeaponVariantDef& variant)
    {
        return FiresProjectiles(variant) && variant.weapDef->weapClass == WEAPCLASS_ROCKETLAUNCHER &&
               variant.weapDef->projectileModel;
    }

    bool FiresBullets(const WeaponVariantDef& variant)
    {
        return variant.weapDef && variant.weapDef->weapType == WEAPTYPE_BULLET;
    }

    XModel* GetStowedModel(const WeaponVariantDef& variant)
    {
        const auto def = variant.weapDef;
        if (!def || !def->worldModel)
            return nullptr;
        // (a launcher and a crossbow have a model for it, what the game's WeaponHasStowedModel asks: the test film's
        // players carry t5_weapon_law_stow, t5_weapon_strela_stow and t5_weapon_mp_crossbow_stow)
        const bool ownModel = def->worldModel[1] && def->weapClass != WEAPCLASS_GRENADE && def->weapClass != WEAPCLASS_ITEM;
        return def->worldModel[ownModel ? 1 : 0];
    }

    bool IsOnlyOnBone(const XSurface& surface, std::uint32_t bone)
    {
        for (std::uint32_t word = 0; word < 5; ++word)
        {
            const auto bit = word == bone / 32 ? 0x80000000u >> (bone % 32) : 0u;
            if ((surface.partBits[word] & ~bit) != 0 || (bit && !(surface.partBits[word] & bit)))
                return false;
        }
        return true;
    }

    std::uint16_t FindMagazineTag(const XModel& model, const WeaponVariantDef& variant)
    {
        const auto rank = [](std::string_view name) {
            for (const auto bigger : {"ext", "double", "dual", "long"})
                if (name.find(bigger) != std::string_view::npos)
                    return 0;
            return name == "tag_clip" ? 1 : 2;
        };
        std::optional<std::pair<int, std::uint16_t>> best;
        for (std::uint32_t bone = 0; bone < model.numBones; ++bone)
        {
            const auto tag = model.boneNames[bone];
            const auto name = Functions::SL_ConvertToString(tag);
            // (not the magazine release, nor an empty one)
            if (name.find("clip") == std::string_view::npos || name.find("release") != std::string_view::npos ||
                name.find("empty") != std::string_view::npos)
                continue;
            bool hidden = false;
            for (std::size_t i = 0; variant.hideTags && i < MAX_WEAPON_HIDE_TAGS && variant.hideTags[i]; ++i)
                hidden |= variant.hideTags[i] == tag;
            const auto& lod = model.lodInfo[0];
            bool drawn = false;
            for (std::uint32_t i = lod.surfIndex; i < static_cast<std::uint32_t>(lod.surfIndex + lod.numsurfs); ++i)
                drawn |= IsOnlyOnBone(model.surfs[i], bone);
            if (!hidden && drawn && (!best || rank(name) < best->first))
                best = std::pair(rank(name), tag);
        }
        return best ? best->second : std::uint16_t{0};
    }

    std::optional<Types::WeaponRig> GetWeaponRig(std::string_view weapon)
    {
        // (the variants and their models are the map's, there until the next one loads; not while a pack loads)
        const auto assets = ContentPacks::TryReadAssets();
        if (!assets.owns_lock() || ContentPacks::IsLoading())
            return std::nullopt;
        const auto variant = FindVariant(weapon);
        const auto model =
            variant && variant->weapDef && variant->weapDef->worldModel ? variant->weapDef->worldModel[0] : nullptr;
        if (!model)
            return std::nullopt;

        // the model's root is on tag_weapon_right, so where its bones are in it is where they are from the tag
        Types::WeaponRig rig{.model = model->name};
        if (const auto tag = FindMagazineTag(*model, *variant))
        {
            const auto pose = AnimationPose::GetRestPose(*model);
            const auto bone = static_cast<std::size_t>(
                std::ranges::find(model->boneNames, model->boneNames + model->numBones, tag) - model->boneNames);
            rig.magazineBone = Functions::SL_ConvertToString(tag);
            rig.magazine = PoseLogic::GetTransform(pose, bone);
        }
        return rig;
    }
}  // namespace IWXMVM::T5::WeaponRig
