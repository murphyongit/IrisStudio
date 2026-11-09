#pragma once
#include "UI/UIComponent.hpp"
#include "Components/Actors.hpp"
#include "Components/Loadouts.hpp"

namespace IWXMVM::UI
{
    // The Scene tab's Players page: the demo's players, for all of them or one, with their death animation, loadout
    // and look (an actor's)
    class PlayersMenu : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        enum class Section
        {
            Look,
            DeathAnimation,
            Loadout,
            LikeAnActor,
        };
        // opens the page with a section open (the harness)
        void Open(Section section);

        // the player or team chosen, or all players (for the harness)
        const Components::Loadouts::Target& GetTarget() const
        {
            return target;
        }
        void SetTarget(const Components::Loadouts::Target& who)
        {
            target = who;
        }

       private:
        void Initialize() final;
        void DrawTargetCombo();
        void DrawLook();
        void DrawFacePaint(const Types::ActorCatalog& catalog, Types::Loadout& loadout);
        void DrawLeftOut();
        void DrawLikeAnActor();

        Components::Loadouts::Target target;
        std::string characterFilter;
        // a section to open and scroll to when the page is drawn next
        std::optional<Section> opening;
        std::string lookMessage;
    };
}  // namespace IWXMVM::UI
