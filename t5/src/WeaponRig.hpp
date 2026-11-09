#pragma once
#include "Types/WeaponRig.hpp"
#include "Structures.hpp"

namespace IWXMVM::T5::WeaponRig
{
    // Where the parts of the guns of the map are, from their world models (see GameInterface::GetWeaponRig)

    // the variant of a weapon by name (as loadouts have it): one the map registered, else one a weapon zone loaded (a
    // campaign or Zombies weapon); null for one neither has
    const Structures::WeaponVariantDef* FindVariant(std::string_view name);

    // the variant of a dual wield weapon the left hand holds (aspdwlh_mp for aspdw_mp), null for a weapon held in one
    // hand or two
    const Structures::WeaponVariantDef* FindLeftHandVariant(const Structures::WeaponVariantDef& variant);

    // the sound alias a shot of the variant plays where others hear it: its own, or for a weapon of the weapon catalog
    // the first of its sounds a bank has (some of the game's names have none); empty for none
    const char* GetShotSound(const Structures::WeaponVariantDef& variant);
    // whether a shot of the variant is a projectile (a Ray Gun's, a launcher's), which no bullet's tracer goes with
    bool FiresProjectiles(const Structures::WeaponVariantDef& variant);
    // whether a shot of the variant is a launcher's rocket, which flies
    bool FiresRockets(const Structures::WeaponVariantDef& variant);
    // whether a shot of the variant is a bullet, which lands where it hits
    bool FiresBullets(const Structures::WeaponVariantDef& variant);

    // the model of the variant that shows on a back (tag_stowed_back), null for none
    Structures::XModel* GetStowedModel(const Structures::WeaponVariantDef& variant);

    // whether a surface's vertices are all on one bone
    bool IsOnlyOnBone(const Structures::XSurface& surface, std::uint32_t bone);

    // The bone of the variant's world model its magazine is: of the bones named like one (tag_clip, tag_ext_clip,
    // tag_clip_long...) that the variant shows and that have surfaces of their own, a bigger one (an extended, long
    // or dual magazine) before tag_clip, and tag_clip before the others; 0 for none
    std::uint16_t FindMagazineTag(const Structures::XModel& model, const Structures::WeaponVariantDef& variant);

    // see GameInterface::GetWeaponRig
    std::optional<Types::WeaponRig> GetWeaponRig(std::string_view weapon);
}  // namespace IWXMVM::T5::WeaponRig
