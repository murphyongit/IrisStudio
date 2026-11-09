#pragma once
#include "UI/UIComponent.hpp"
#include "Components/Actors.hpp"
#include "Components/Loadouts.hpp"
#include "Types/DemoEvents.hpp"

namespace IWXMVM::UI
{
    // The demo's players' death animations, on the Scene tab's Players page (see PlayersMenu): the animation for all
    // players or the one chosen there, with a preview, the players with their own, and what corpses do
    class PlayerAnimation : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        // what the Players page shows of it, for all players or one (which a click on one of those with their own
        // animation picks)
        void RenderContent();
        void SetTarget(const Components::Loadouts::Target& who)
        {
            target = who;
        }
        const Components::Loadouts::Target& GetTarget() const
        {
            return target;
        }

       private:
        void Initialize() final;

        void DrawAnimationList(ImVec2 size);
        void DrawPreview(ImVec2 size);
        void DrawBrowse();
        void DrawPlayerAnimations(const Types::DemoEvents* events);

        // who the animation list chooses for
        Components::Loadouts::Target target;
        // the animation browser, and how many deaths it has (of the catalog it counted)
        std::string browsedAnimation, browserFilter, browserCategory;
        std::size_t catalogDeaths = 0;
        std::shared_ptr<const Types::ActorCatalog> countedCatalog;
        bool favoritesOnly = false;
        std::array<char, 64> filter{};
        std::optional<std::string_view> hoveredAnimation;

        // the preview: the animation it plays and since when, which way it's turned and the area the animation covers
        std::optional<std::uint32_t> previewAnimation;
        double previewStart = 0.0;
        float previewYaw = 0.7f;
        std::optional<std::pair<ImVec2, ImVec2>> previewBounds;
        float previewBoundsYaw = 0.0f;
    };
}  // namespace IWXMVM::UI
