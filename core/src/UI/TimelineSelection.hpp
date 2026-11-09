#pragma once
#include <set>

#include "Types/Actor.hpp"
#include "Types/Keyframe.hpp"
#include "UI/TimelineMath.hpp"
#include "UI/PanelLogic.hpp"

// What is selected on the timeline: campath nodes and keys, actors' clips, events and keys, the bars of effects and
// lights, and markers. The view and
// the side panel share it (see Timeline): a node clicked in the view or a clip picked on the Actors tab becomes the
// selection, and a node or clip selected alone here is the view's and the panel's. Delete, Duplicate, Copy, Paste and
// moves act on it, each as one step to undo, with a message. All on the render thread, which draws the UI.
namespace IWXMVM::UI::TimelineSelection
{
    enum class Kind : std::uint8_t
    {
        Key,       // owner: the property's type; id: the keyframe's
        Clip,      // owner: the actor's id; id: the clip's
        Event,     // owner: the actor's id; id: the event's
        ActorKey,  // owner: the actor's id; id: the key's tick
        Marker,    // id: the marker's
        FaceClip,  // owner: the actor's id; id: the clip's of its Face row
        // owner: the actor's id: its bar, which stands for all of it (Delete takes the effect, light or prop itself)
        EffectBar,
        LightBar,
        PropBar,
    };

    struct Item
    {
        Kind kind = Kind::Key;
        std::uint32_t owner = 0;
        std::int64_t id = 0;

        auto operator<=>(const Item&) const = default;
    };

    Item KeyItem(Types::KeyframeablePropertyType property, std::int32_t keyframeId);
    Item BarItem(const Types::Actor& actor);
    bool IsBar(const Item& item);
    // what an item is to the side panel, which shows it (a key of the campath is a node)
    PanelLogic::Kind GetPanelKind(const Item& item);

    const std::set<Item>& Get();
    bool IsSelected(const Item& item);
    // replaces the selection
    void Set(std::span<const Item> items);
    void Add(std::span<const Item> items);
    // Ctrl+click: in or out
    void Toggle(const Item& item);
    // nothing selected (and a move going on forgotten, as when another demo loads)
    void Clear();
    // what Shift+click selects from: the item clicked last
    std::optional<Item> GetAnchor();
    void SetAnchor(std::optional<Item> item);

    // how many of the selected items still exist (an undo can take keys away); Prune forgets those that don't
    std::size_t Count();
    void Prune();
    // the campath node selected, when it is all that is selected
    std::optional<std::int32_t> GetOnlyNode();

    // where an item is on the timeline, while it exists
    std::optional<TimelineMath::Span> GetSpan(const Item& item);
    // what items are called: "2 nodes", "key", "3 clips", "4 items"
    std::string Describe(std::span<const Item> items);

    // Edits of the selection, each one step to undo, with a message; they return how many items they did.
    // Duplicate puts the copies right after the selection; Paste puts what Copy took with its first at the tick.
    // The copies and pasted items become the selection.
    std::size_t Delete();
    std::size_t Duplicate();
    std::size_t Copy();
    std::size_t Paste(std::uint32_t tick);
    std::size_t CountCopied();
    // forgets what was copied (another demo's actors are others)
    void ForgetCopied();

    // A drag of the selection: BeginMove where it starts, UpdateMove by how far it is from there as it goes on, and
    // EndMove makes it one step. Keys that land on another key's tick are put a tick further (TimelineMath::
    // SeparateTicks).
    void BeginMove();
    void UpdateMove(std::int64_t delta);
    void EndMove();
    bool IsMoving();
    // where the items being moved were when the drag started
    std::vector<TimelineMath::Span> GetMovingSpans();
    // a move at once, as one step (Alt+arrows)
    void MoveBy(std::int64_t delta);

    // What the keys selected (nodes and keys of properties) have in common: an interpolation when they all have it,
    // whether one is Auto (an older file's), whether all ease in, all ease out, and how much when they all do as much
    struct KeyOptions
    {
        std::size_t keys = 0;
        std::optional<Types::KeyInterpolation> interpolation;
        bool anyAuto = false;
        bool easedIn = false;
        bool easedOut = false;
        std::optional<float> easeIn;
        std::optional<float> easeOut;
    };
    KeyOptions GetKeyOptions();
    // The interpolation, or how much they ease in or out (0 to 1), of the keys selected, as one step with a message
    // (none for the changes of a drag of the amount but its last); they return how many keys they changed
    std::size_t SetInterpolation(Types::KeyInterpolation interpolation);
    std::size_t SetEase(bool in, float amount, bool message = true);
}  // namespace IWXMVM::UI::TimelineSelection
