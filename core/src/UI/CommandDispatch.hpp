#pragma once

// Which command a key press runs, apart from ImGui so it can be tested (see Commands.hpp for the commands
// themselves). Keys reach the editor's input contexts from the highest down; a context that uses a key keeps it from
// those below.
namespace IWXMVM::UI::CommandDispatch
{
    // the input contexts, highest first
    enum class Context : std::uint8_t
    {
        Typing,     // a text field is active
        Popup,      // a popup or modal is open
        Capturing,  // recording
        Fly,        // the free camera has the keyboard and mouse
        Placing,    // clicks in the view place path points
        Viewport,   // the mouse is over the view
        Timeline,   // the mouse is over the timeline (or it has the focus)
        Panel,      // the side panel has the focus
        Global,     // always

        Count,
    };

    using ContextMask = std::uint32_t;

    constexpr ContextMask Bit(Context context)
    {
        return 1u << static_cast<std::uint32_t>(context);
    }

    constexpr ContextMask ALL_CONTEXTS = (1u << static_cast<std::uint32_t>(Context::Count)) - 1;

    // Typing, Popup, Capturing and Fly take every key: what they don't use does nothing
    bool TakesEveryKey(Context context);

    // The contexts keys reach: the active ones from the highest down to the first that takes every key
    ContextMask Reachable(ContextMask active);

    // A key with the modifiers held with it, with the same bits as ImGuiKeyChord: the key in the low bits
    using Chord = std::int32_t;
    constexpr Chord CTRL = 1 << 12;
    constexpr Chord SHIFT = 1 << 13;
    constexpr Chord ALT = 1 << 14;
    constexpr Chord SUPER = 1 << 15;
    constexpr Chord MODIFIERS = CTRL | SHIFT | ALT | SUPER;

    constexpr std::int32_t KeyOf(Chord chord)
    {
        return chord & ~MODIFIERS;
    }

    constexpr Chord ModifiersOf(Chord chord)
    {
        return chord & MODIFIERS;
    }

    // What dispatching needs to know of a command
    struct Binding
    {
        std::vector<Chord> chords;
        ContextMask contexts = 0;
        // a held press runs it again at the key repeat rate (skips, nudges); else once a press
        bool repeat = false;
        // a key to hold (W flies forward): matched with any modifiers (they only change the speed) and never run
        bool held = false;
    };

    // A key pressed this frame; repeat when it was already down (the key repeat of a held key)
    struct Press
    {
        std::int32_t key = 0;
        bool repeat = false;
    };

    // Whether a press with the modifiers held matches a chord of a binding: the same key with exactly the same
    // modifiers (Ctrl+D is not Ctrl+Shift+D, and not D), or the key with any modifiers for a held binding
    bool Matches(const Binding& binding, std::int32_t key, Chord modifiers);

    // What a press does: from the highest reachable context down, the first context with a binding that matches
    // (and is available, as available says of its index) uses the key, and that binding runs, unless the press is a
    // repeat it doesn't take or it is held. A key no context uses runs nothing.
    struct Outcome
    {
        std::optional<std::size_t> run;  // the binding to run
        bool used = false;               // a context used the key (whether or not anything runs)
    };
    Outcome Dispatch(std::span<const Binding> bindings, Press press, Chord modifiers, ContextMask active,
                     const std::function<bool(std::size_t)>& available);

    // Whether a held binding is held: one of its keys is down, whatever the modifiers, while one of its contexts is
    // reachable
    bool IsHeld(const Binding& binding, ContextMask active, const std::function<bool(std::int32_t key)>& keyDown);

    // Two bindings conflict when a press could match both in a context they share: a chord in common, or the key of
    // a held one in a chord of the other
    bool Conflict(const Binding& a, const Binding& b);

    // The bindings a candidate conflicts with (a binding as it would be with other chords), but skip, its own index:
    // in the order Dispatch tries them in a context they share, where the first that is available runs
    std::vector<std::size_t> FindConflicts(std::span<const Binding> bindings, const Binding& candidate,
                                           std::optional<std::size_t> skip = std::nullopt);

    // How many chords could run more than one binding: each counted once, however
    // many bindings share it
    std::size_t CountSharedChords(std::span<const Binding> bindings);

    // One of the bindings that share a key, for saying which runs
    struct ClashName
    {
        std::string_view label;
        // the maps it works on when that isn't all of them ("campaign and Zombies maps"), else empty
        std::string_view maps;
        bool self = false;  // the one the words are about: "this"
    };
    // Which runs, in words, of bindings that share a key, in the order Dispatch tries them: "On campaign and Zombies
    // maps Go to Area runs. On others this does."
    std::string DescribeClash(std::span<const ClashName> names);
}  // namespace IWXMVM::UI::CommandDispatch
