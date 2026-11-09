#pragma once
#include "UI/UIComponent.hpp"
#include "Types/Actor.hpp"
#include "Components/Actors.hpp"
#include "Components/ReloadLogic.hpp"
#include "UI/PanelLogic.hpp"

namespace IWXMVM::UI
{
    // the time of a timeline tick as the actors UI shows it, e.g. "12.35 s"
    std::string FormatActorTime(std::uint32_t tick);

    // goes to a tick of the timeline, as the keyframe arrows do
    void JumpTo(std::uint32_t tick);

    // Ends the step of changes to the actors that one undo takes back, unless something is being dragged or typed;
    // every UI that changes actors calls it each frame
    void EndActorEditStepWhenIdle();

    // A window of every animation the game has, by where they are from and sorted into categories, with favourites, a
    // search, a filter of those that fit the actor's models (the first, its body, shows them) and a preview, opened with
    // ImGui::OpenPopup(popupId); returns whether one was chosen into choice (a double-click or Use). Those the game
    // hasn't loaded are listed as such and can't be chosen. With facesOnly, of the animations of the face alone (for the
    // Face row). Its animations can be dragged onto actors' lanes on the timeline, as a drag and drop payload of this
    // type (the animation's name, ending in a zero): those of the face onto the Face row
    inline constexpr const char* ANIMATION_PAYLOAD = "IWXMVM_ACTOR_ANIMATION";
    bool AnimationBrowser(const char* popupId, const Types::ActorCatalog& catalog, const std::vector<std::string>& models,
                          std::string& choice, std::string& filter, std::string& category, bool facesOnly = false);
    // The characters to choose from in a combo (id) showing preview: the favourites, the map's own, then those of each
    // level, with a search. A first row keep, if any, chooses none; peopleOnly leaves out dogs and killstreaks. Calls
    // choose with what was chosen, null for keep. The combo is recorded for the harness as record and its rows as
    // <rowRecord>.<id>.
    void CharacterPicker(const char* id, const Types::ActorCatalog& catalog, const Types::Character* current,
                         const std::string& preview, const char* keep, std::string& filter, const std::string& record,
                         const std::string& rowRecord, bool peopleOnly, const char* tooltip,
                         const std::function<void(const Types::Character*)>& choose);
    // The game's face paints as rows of a list or an open combo, the one at current selected, hovered set to the one
    // under the mouse to show it live; a first row keep, if any, is -1. Returns the one clicked. Recorded for the harness
    // as <record>.<number>.
    std::optional<std::int32_t> FacePaintRows(std::int32_t current, const char* keep,
                                              std::optional<std::int32_t>& hovered, const std::string& record);

    // A window of the effects the game has loaded, by the categories of their folders,
    // with favourites, the recent ones and a search, each marked Loops or Once as it plays by itself; opened with
    // ImGui::OpenPopup(popupId). Returns whether one was chosen into choice (a double-click, or the button labelled
    // use).
    bool EffectBrowser(const char* popupId, const Types::ActorCatalog& catalog, std::string& choice,
                       std::string& filter, std::string& category, const char* use);

    // A window of the models the game has loaded: by group, the map's own
    // first, with a search, opened with ImGui::OpenPopup(popupId) on the side of the screen away from keepClear (where
    // the model will show). Hovering a row stands its model where the ray from origin hits, facing yaw, till the mouse
    // leaves the row (DE1). Returns whether one was chosen into choice (a double-click, Enter, or the button labelled
    // use). Its group is kept in group by key ("map").
    struct PropPreviewRay
    {
        glm::vec3 origin{0.0f};
        glm::vec3 direction{1.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
    };
    bool PropBrowser(const char* popupId, const Types::ActorCatalog& catalog, std::string& choice, std::string& filter,
                     std::string& group, const char* use, const PropPreviewRay& preview, ImVec2 keepClear);

    // opens every section of the Actors panel when it is drawn next, and turns its Advanced switch off (the harness, as
    // each scenario starts)
    void ResetActorPanel();

    // what the browser drawn last showed, for the harness
    struct AnimationBrowserState
    {
        std::size_t shown = 0;   // animations, with its filters
        std::size_t listed = 0;  // in all
        bool fitting = false;    // only those that fit the actor's models (Components::Actors::IsFitFilterOn)
        bool fitsKnown = false;  // which those are is known (the game has loaded the models)
        bool waiting = false;    // it shows none until the models have loaded
        int frame = -1;          // ImGui's frame it was drawn in
    };
    AnimationBrowserState GetAnimationBrowserState();
    // the color of animations that don't fit an actor where they are listed all the same (the fit filter off): the
    // palette's (UI/Theme.hpp)
    ImVec4 MisfitColor();

    // Then...: the items of an open menu or popup of what can follow one of an actor's clips (see
    // Components::Actors::GetChainCandidates): the transitions of its movement set first, then the catalog's others
    // (cover arrivals and exits among them), each with what it does and, where known, how far its first pose is from
    // the clip's last; choosing one plays it after the clip (ActorActions::PlayAfter). The items are recorded for the
    // harness as <record>.<animation>. Returns whether one was chosen.
    bool DrawThenItems(const Types::Actor& actor, std::uint32_t clipId, const std::string& record);

    // The actors of the demo (see Components::Actors): the list, and the selected one in one panel:
    // its look (character, weapon), movement (its path's gaits), animation clips and events, each with the
    // options used most, and with each section's Advanced the rest
    class ActorsMenu : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        // Delete (Ctrl+D) on the section last clicked or picked in (the list's: the actor): the selected clip, event,
        // path point or key at the playhead, or the actor itself; why it can't now, nothing when it can
        std::optional<std::string> GetEditUnavailableReason(bool remove);
        void DeleteOrDuplicateSelected(bool remove);
        // opens the effect browser to add one, on the Scene tab
        void AddEffect();
        // opens the prop browser to add one where the camera looks, on the Scene tab
        void AddProp();

        using Page = PanelLogic::ActorPage;
        // opens and scrolls to a section of the selected actor's when it is drawn next (a clip clicked again on the
        // timeline)
        void ShowPage(Page shown);

        // What a change on the piece of a walk shown in the Animation section reaches (Apply to),
        // as it was drawn last, for the timeline to outline: the piece's stretches, or the whole
        // path; none while no piece is shown
        struct PieceScope
        {
            std::uint32_t actor = 0;
            std::size_t first = 0;
            std::size_t last = 0;
            bool wholePath = false;
        };
        std::optional<PieceScope> GetPieceScope() const
        {
            return pieceScope;
        }

       private:
        void Initialize() final;
        void RenderPackMenu(const Types::ActorCatalog& catalog);
        void RenderPackState(const Types::ActorCatalog& catalog);
        void RenderList();
        void RenderActorMenu(const Types::Actor& actor);
        void RenderInspector();
        void RenderLook(Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderFacePaint(Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderCharacterPicker(Types::Actor& actor, const Types::ActorCatalog& catalog,
                                   const Types::Character* current);
        void RenderCharacterLook(Types::Actor& actor, const Types::ActorCatalog& catalog,
                                 const Types::Character& character);
        void RenderParts(Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderZoneState(const Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderZoneSetup();
        void RenderClips(Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderLegs(const Types::Actor& actor);
        void RenderClipWalk(const Types::Actor& actor, Types::ActorClip& clip);
        void RenderClipWalkDetails(const Types::Actor& actor, Types::ActorClip& clip);
        std::optional<ActorLogic::ClipAfter> RenderClipAfter(const Types::Actor& actor, const Types::ActorClip& clip);
        void RenderClipRootMotion(const Types::Actor& actor, Types::ActorClip& clip);
        void RenderFaceClip(const Types::Actor& actor, Types::ActorClip& clip, const Types::ActorCatalog& catalog);
        void RenderLocomotion(Types::Actor& actor);
        void RenderStance(Types::Actor& actor);
        void RenderMovementSet(Types::Actor& actor);
        void RenderSpeed(Types::Actor& actor, const ActorLogic::Body& body);
        void RenderVehicle(Types::Actor& actor, const ActorLogic::Body& body);
        void RenderReload(const Types::Actor& actor, const Types::ActorClip& clip,
                          const Types::ActorCatalog::Animation& animation, std::uint32_t end);
        void RenderMovement(Types::Actor& actor);
        void RenderPath(Types::Actor& actor, const ActorLogic::Body& body);
        void RenderGaits(Types::Actor& actor, std::optional<std::size_t> point);
        // a walk's piece in the Animation section: its look, gait, speed and blend
        void RenderWalkPiece(Types::Actor& actor, std::uint32_t clipId, const ActorLogic::WalkPiece& piece);
        void RenderApplyTo(const char* id, const char* record);
        void RenderLookRow(Types::Actor& actor, const ActorLogic::WalkPiece& piece);
        void RenderNotes(Types::Actor& actor);
        void RenderWeapon(Types::Actor& actor, const char* none = "None", std::string* search = nullptr);
        void RenderStowed(Types::Actor& actor);
        void RenderLive();
        void RenderYou();
        void RenderLiveRow(std::uint32_t id, const std::string& name, const std::string& detail,
                           std::optional<bool> alive, bool selected, const std::function<void()>& remove);
        void RenderEvents(Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderDeath(Types::Actor& actor, Types::ActorEvent& hit, const Types::ActorCatalog& catalog);
        void RenderLookAt(Types::Actor& actor, Types::ActorEvent& look);
        void RenderEffect(Types::ActorEvent& event, const Types::ActorCatalog& catalog);
        void RenderProps(Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderAttachment(Types::Actor& actor);
        void RenderAttachmentDetails(Types::Actor& actor);
        void RenderPresets(Components::Actors::PresetKind kind, Types::Actor& actor);
        void RenderCopyLook(const Types::Actor& actor);
        void RenderGroup(const Types::Actor& actor);
        void DeleteOrDuplicate(Types::Actor& actor, bool remove);
        // an effect in the scene's sections, in place of an actor's
        void RenderSceneEffect(Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderEffectBrowser();
        // a light in the scene's sections, in place of an actor's
        void RenderSceneLight(Types::Actor& actor);
        // a prop in the scene's sections, in place of an actor's
        void RenderSceneProp(Types::Actor& actor, const Types::ActorCatalog& catalog);
        // the Mortar Team's: its points, the way its shells come, and its events
        void RenderSceneMortar(Types::Actor& actor, const Types::ActorCatalog& catalog);
        void RenderPropBrowser();

        Page page = Page::Look;
        std::optional<Page> requestedPage;
        // a folded section whose Advanced was turned on, opened when it is drawn next
        std::optional<Page> advancedOpened;
        // a point picked on the timeline or in the view: its stretch's row in Movement is scrolled to and flashes
        // a moment
        std::chrono::steady_clock::time_point pointShownAt;
        bool scrollToPoint = false;
        // the selection last seen, to open the page of what was picked on the timeline or in the view
        std::optional<std::uint32_t> seenActor;
        std::optional<std::uint32_t> seenClip;
        std::optional<std::uint32_t> seenFaceClip;
        std::optional<std::uint32_t> seenEvent;
        std::optional<std::size_t> seenWaypoint;
        // an actor's name being typed in its place in the list
        std::optional<std::uint32_t> renaming;
        std::string renameBuffer;
        bool renameFocus = false;
        // Apply to: whether a change on the piece shown goes on its whole path (else
        // its stretches), This stretch again each time another piece is shown; that piece (its actor, clip and first
        // stretch), and what it reaches as drawn last
        bool applyToPath = false;
        std::optional<std::tuple<std::uint32_t, std::uint32_t, std::size_t>> shownPiece;
        std::optional<PieceScope> pieceScope;
        // Try another: the actor as it was before a look in the list was
        // previewed (hovered or stepped to), and the row stepped to
        std::optional<Types::Actor> lookPreviewWas;
        // the actor as it was before a face paint was hovered in Face paint's list
        std::optional<Types::Actor> facePaintPreviewWas;
        int lookRow = -1;
        // the browser opened from the look list (its Browse...), and what it shows chosen
        bool openLookBrowser = false;
        std::string lookBrowsed;

        std::string animationFilter;
        std::string animationCategory = "All";
        std::string faceCategory = "All";  // of the Face row's browser
        // what the Animation page's Browse chose, to play from the playhead
        std::string browsed;
        std::string modelFilter;
        std::string characterFilter;
        std::string liveWeaponFilter;
        // the You page's gun, before Give
        Types::Actor youGun;
        std::string youWeaponFilter;
        std::string effectFilter;
        // the effect browser: opened next frame, to add the one chosen (Add > Effect...) or to change the selected
        // effect's; what it chose, its search and category
        bool openEffectBrowser = false;
        bool effectBrowserAdds = false;
        std::string effectBrowsed;
        std::string effectBrowserFilter;
        std::string effectBrowserCategory = "All";
        // the prop browser, the same: to add the one chosen where the camera looks, or to change the selected prop's
        bool openPropBrowser = false;
        bool propBrowserAdds = false;
        std::string propBrowsed;
        std::string propBrowserFilter;
        std::string propBrowserGroup = "map";
        std::string propFilter;
        std::string presetName;
        std::string copyLookMessage;
        ActorLogic::Formation formation;
        std::string deathAnimation;
        // a reload marker being dragged: the override it previews, and the frame it was dragged in last
        struct ReloadPreview
        {
            std::string animation;
            ReloadLogic::ProfileEntry entry;
            int frame = 0;
        };
        std::optional<ReloadPreview> reloadPreview;
        // the marker the reload strip's menu is for
        std::optional<ReloadLogic::Moment> reloadMenu;
    };
}  // namespace IWXMVM::UI
