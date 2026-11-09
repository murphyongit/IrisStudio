#pragma once
#include "UI/UIComponent.hpp"
#include "Components/Loadouts.hpp"
#include "Types/DemoEvents.hpp"
#include "Types/Loadout.hpp"

#include <set>

namespace IWXMVM::UI
{
    // The demo's players' loadouts, on the Scene tab's Players page (see PlayersMenu): the character and weapons for
    // all players or the one chosen there
    class Loadouts : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        // what the Players page shows of it, for all players or one
        void RenderContent();
        void SetTarget(const Components::Loadouts::Target& who)
        {
            target = who;
        }
        // the body and head models to choose from, on the Players page's Look section
        void DrawParts(const Types::LoadoutCatalog& catalog, Types::Loadout& loadout);
        // the gun on their back: keep (what the demo gives them, or the choice for all players), off, or a gun
        void DrawStowed(const Types::LoadoutCatalog& catalog, Types::Loadout& loadout, const char* keep);

       private:
        void Initialize() final;

        void DrawWeaponRule(const Types::LoadoutCatalog& catalog, Types::Loadout& loadout,
                            const std::set<std::string>& demoWeapons);
        void DrawRules(const Types::LoadoutCatalog& catalog, Types::Loadout& loadout);

        // who the choices are for
        Components::Loadouts::Target target;
        // the weapon held in the demo whose rule is edited (see WeaponRule::from)
        std::string editedFrom{Types::WeaponRule::ANY_PRIMARY};
    };
}  // namespace IWXMVM::UI
