#include "StdInclude.hpp"
#include "Test.hpp"

#include "UI/CommandDispatch.hpp"

using namespace IWXMVM::UI::CommandDispatch;

namespace
{
    // keys as ImGui numbers them, for readable tests (the dispatch doesn't care)
    constexpr std::int32_t KEY_D = 549;
    constexpr std::int32_t KEY_K = 556;
    constexpr std::int32_t KEY_W = 568;
    constexpr std::int32_t KEY_Z = 571;
    constexpr std::int32_t KEY_SPACE = 524;
    constexpr std::int32_t KEY_ESCAPE = 526;
    constexpr std::int32_t KEY_DELETE = 522;
    constexpr std::int32_t KEY_RIGHT = 514;
    constexpr std::int32_t KEY_G = 552;
    constexpr std::int32_t KEY_M = 558;
    constexpr std::int32_t KEY_LEFT_BRACKET = 591;
    constexpr std::int32_t KEY_RIGHT_BRACKET = 593;

    constexpr ContextMask EVERYWHERE = Bit(Context::Fly) | Bit(Context::Global);

    std::optional<std::size_t> Run(const std::vector<Binding>& bindings, std::int32_t key, Chord modifiers,
                                   ContextMask active, bool repeat = false,
                                   std::function<bool(std::size_t)> available = [](std::size_t) { return true; })
    {
        return Dispatch(bindings, {key, repeat}, modifiers, active, available).run;
    }
}  // namespace

TEST("a chord matches its modifiers exactly")
{
    const std::vector<Binding> bindings = {
        {{CTRL | KEY_D}, Bit(Context::Global)},
    };
    const auto global = Bit(Context::Global);
    CHECK(Run(bindings, KEY_D, CTRL, global) == 0);
    CHECK(!Run(bindings, KEY_D, CTRL | SHIFT, global));
    CHECK(!Run(bindings, KEY_D, CTRL | ALT, global));
    CHECK(!Run(bindings, KEY_D, 0, global));
    // another key with the same modifiers
    CHECK(!Run(bindings, KEY_K, CTRL, global));
}

TEST("presses don't repeat unless the command does")
{
    const std::vector<Binding> bindings = {
        {{CTRL | KEY_D}, Bit(Context::Global)},
        {{KEY_RIGHT}, Bit(Context::Global), true},
    };
    const auto global = Bit(Context::Global);
    CHECK(Run(bindings, KEY_D, CTRL, global, false) == 0);
    CHECK(!Run(bindings, KEY_D, CTRL, global, true));
    // a skip goes on while the key is held
    CHECK(Run(bindings, KEY_RIGHT, 0, global, true) == 1);
}

TEST("flying takes every key: Ctrl+D is flying right slowly, not a duplicate")
{
    const std::vector<Binding> bindings = {
        {{KEY_D}, Bit(Context::Fly), false, true},                         // fly right, held
        {{CTRL | KEY_D}, Bit(Context::Viewport) | Bit(Context::Global)},  // duplicate
        {{KEY_DELETE}, Bit(Context::Viewport) | Bit(Context::Global)},    // delete
        {{KEY_K}, EVERYWHERE},                                             // add a node
    };
    const auto flying = Bit(Context::Fly) | Bit(Context::Viewport) | Bit(Context::Global);
    // the held key is used whatever the modifiers, and nothing runs
    auto outcome = Dispatch(bindings, {KEY_D, false}, CTRL, flying, [](std::size_t) { return true; });
    CHECK(outcome.used);
    CHECK(!outcome.run);
    // a key flying doesn't use goes nowhere
    outcome = Dispatch(bindings, {KEY_DELETE, false}, 0, flying, [](std::size_t) { return true; });
    CHECK(!outcome.used);
    CHECK(!outcome.run);
    // but what flying has does run
    CHECK(Run(bindings, KEY_K, 0, flying) == 3);
    // not flying, Ctrl+D duplicates
    CHECK(Run(bindings, KEY_D, CTRL, Bit(Context::Viewport) | Bit(Context::Global)) == 1);
}

TEST("a key a context uses isn't seen by those below it")
{
    const std::vector<Binding> bindings = {
        {{KEY_ESCAPE}, Bit(Context::Global)},     // e.g. close a window
        {{KEY_ESCAPE}, Bit(Context::Placing)},    // stop placing
        {{KEY_SPACE}, Bit(Context::Global)},      // play
    };
    const auto placing = Bit(Context::Placing) | Bit(Context::Viewport) | Bit(Context::Global);
    CHECK(Run(bindings, KEY_ESCAPE, 0, placing) == 1);
    CHECK(Run(bindings, KEY_ESCAPE, 0, Bit(Context::Global)) == 0);
    // placing doesn't take every key: Space still plays
    CHECK(Run(bindings, KEY_SPACE, 0, placing) == 2);
}

TEST("typing and popups take every key")
{
    const std::vector<Binding> bindings = {
        {{KEY_SPACE}, EVERYWHERE},
        {{CTRL | KEY_Z}, Bit(Context::Global)},
    };
    const auto typing = Bit(Context::Typing) | Bit(Context::Panel) | Bit(Context::Global);
    CHECK(!Run(bindings, KEY_SPACE, 0, typing));
    CHECK(!Run(bindings, KEY_Z, CTRL, typing));
    const auto popup = Bit(Context::Popup) | Bit(Context::Global);
    CHECK(!Run(bindings, KEY_SPACE, 0, popup));
    CHECK(Reachable(typing | popup) == Bit(Context::Typing));
    CHECK(Reachable(Bit(Context::Viewport) | Bit(Context::Global)) == (Bit(Context::Viewport) | Bit(Context::Global)));
}

TEST("an unavailable command leaves the key to the contexts below")
{
    const std::vector<Binding> bindings = {
        {{KEY_DELETE}, Bit(Context::Timeline)},  // delete the selected keys
        {{KEY_DELETE}, Bit(Context::Global)},    // delete the selected actor
    };
    const auto overTimeline = Bit(Context::Timeline) | Bit(Context::Global);
    CHECK(Run(bindings, KEY_DELETE, 0, overTimeline) == 0);
    // no keys selected
    CHECK(Run(bindings, KEY_DELETE, 0, overTimeline, false, [](std::size_t index) { return index != 0; }) == 1);
    // nothing selected at all
    const auto outcome =
        Dispatch(bindings, {KEY_DELETE, false}, 0, overTimeline, [](std::size_t) { return false; });
    CHECK(!outcome.run);
    CHECK(!outcome.used);
}

TEST("held keys are held with any modifiers, while their context is reachable")
{
    const Binding forward = {{KEY_W}, Bit(Context::Fly), false, true};
    const auto down = [](std::int32_t key) { return key == KEY_W; };
    CHECK(IsHeld(forward, Bit(Context::Fly) | Bit(Context::Global), down));
    // not flying
    CHECK(!IsHeld(forward, Bit(Context::Viewport) | Bit(Context::Global), down));
    // a text field above it
    CHECK(!IsHeld(forward, Bit(Context::Typing) | Bit(Context::Fly), down));
    CHECK(!IsHeld(forward, Bit(Context::Fly), [](std::int32_t) { return false; }));
    // a press binding is never held
    const Binding press = {{KEY_W}, Bit(Context::Fly)};
    CHECK(!IsHeld(press, Bit(Context::Fly), down));
}

TEST("conflicts: a chord in common in a context both have")
{
    const Binding addNode = {{KEY_K}, EVERYWHERE};
    const Binding play = {{KEY_K}, Bit(Context::Global)};
    const Binding stopPlacing = {{KEY_K}, Bit(Context::Placing)};
    const Binding ctrlK = {{CTRL | KEY_K}, Bit(Context::Global)};
    CHECK(Conflict(addNode, play));
    // contexts above and below each other are meant to share keys
    CHECK(!Conflict(play, stopPlacing));
    CHECK(!Conflict(play, ctrlK));
    // a held key takes the key with every modifier
    const Binding flyRight = {{KEY_D}, Bit(Context::Fly), false, true};
    const Binding duplicate = {{CTRL | KEY_D}, Bit(Context::Fly) | Bit(Context::Global)};
    CHECK(Conflict(flyRight, duplicate));
    CHECK(Conflict(duplicate, flyRight));
    // a command without a key conflicts with nothing
    const Binding none = {{0}, Bit(Context::Global)};
    const Binding alsoNone = {{0}, Bit(Context::Global)};
    CHECK(!Conflict(none, alsoNone));
}

namespace
{
    // The six commands as registered (the view's before the timeline's):
    // Go to Area on G; the area steps only while flying or with the map open
    enum Keys : std::size_t
    {
        GO_TO_AREA,
        PREVIOUS_AREA,
        NEXT_AREA,
        ADD_MARKER,
        KEY_BEFORE,
        KEY_AFTER,
    };
    const std::vector<Binding> PICKED = {
        {{KEY_G}, Bit(Context::Fly) | Bit(Context::Popup) | Bit(Context::Global)},
        {{KEY_LEFT_BRACKET}, Bit(Context::Fly) | Bit(Context::Popup)},
        {{KEY_RIGHT_BRACKET}, Bit(Context::Fly) | Bit(Context::Popup)},
        {{KEY_M}, EVERYWHERE},
        {{KEY_LEFT_BRACKET}, Bit(Context::Global), true},
        {{KEY_RIGHT_BRACKET}, Bit(Context::Global), true},
    };

    constexpr auto PLAYING = Bit(Context::Global);
    constexpr auto FLYING = Bit(Context::Fly) | Bit(Context::Viewport) | Bit(Context::Global);
    constexpr auto POPUP = Bit(Context::Popup) | Bit(Context::Global);

    // the area commands run only on campaign and Zombies maps, and in a popup only when it is the map
    std::function<bool(std::size_t)> OnMap(bool areas, bool mapOpen = false)
    {
        return [=](std::size_t index) {
            if (index > NEXT_AREA)
                return true;
            return areas && mapOpen;
        };
    }
    std::function<bool(std::size_t)> OnMapPlaying(bool areas)
    {
        return [=](std::size_t index) { return index > NEXT_AREA || areas; };
    }
}  // namespace

TEST("CL1: M adds a marker on every map, G goes to an area")
{
    for (const bool areas : {false, true})
    {
        CHECK(Run(PICKED, KEY_M, 0, PLAYING, false, OnMapPlaying(areas)) == ADD_MARKER);
        CHECK(Run(PICKED, KEY_M, 0, FLYING, false, OnMapPlaying(areas)) == ADD_MARKER);
    }
    CHECK(Run(PICKED, KEY_G, 0, PLAYING, false, OnMapPlaying(true)) == GO_TO_AREA);
    CHECK(Run(PICKED, KEY_G, 0, FLYING, false, OnMapPlaying(true)) == GO_TO_AREA);
    // a multiplayer map: G is nobody's, so it stays the game's
    const auto outcome = Dispatch(PICKED, {KEY_G, false}, 0, PLAYING, OnMapPlaying(false));
    CHECK(!outcome.run);
    CHECK(!outcome.used);
    // the map open: G closes it, M does nothing
    CHECK(Run(PICKED, KEY_G, 0, POPUP, false, OnMap(true, true)) == GO_TO_AREA);
    CHECK(!Run(PICKED, KEY_M, 0, POPUP, false, OnMap(true, true)));
    // another popup (a menu, the key popup of Controls): G isn't the map's
    CHECK(!Run(PICKED, KEY_G, 0, POPUP, false, OnMap(true, false)));
}

TEST("CM1: [ and ] step through the areas flying or with the map open, else go to the keys before and after")
{
    for (const bool areas : {false, true})
    {
        CHECK(Run(PICKED, KEY_LEFT_BRACKET, 0, PLAYING, false, OnMapPlaying(areas)) == KEY_BEFORE);
        CHECK(Run(PICKED, KEY_RIGHT_BRACKET, 0, PLAYING, false, OnMapPlaying(areas)) == KEY_AFTER);
        // held, the key jumps go on
        CHECK(Run(PICKED, KEY_RIGHT_BRACKET, 0, PLAYING, true, OnMapPlaying(areas)) == KEY_AFTER);
    }
    CHECK(Run(PICKED, KEY_LEFT_BRACKET, 0, FLYING, false, OnMapPlaying(true)) == PREVIOUS_AREA);
    CHECK(Run(PICKED, KEY_RIGHT_BRACKET, 0, FLYING, false, OnMapPlaying(true)) == NEXT_AREA);
    CHECK(Run(PICKED, KEY_LEFT_BRACKET, 0, POPUP, false, OnMap(true, true)) == PREVIOUS_AREA);
    CHECK(Run(PICKED, KEY_RIGHT_BRACKET, 0, POPUP, false, OnMap(true, true)) == NEXT_AREA);
    // flying on a multiplayer map, or with a menu open: nothing, as before
    CHECK(!Run(PICKED, KEY_LEFT_BRACKET, 0, FLYING, false, OnMapPlaying(false)));
    CHECK(!Run(PICKED, KEY_LEFT_BRACKET, 0, POPUP, false, OnMap(true, false)));
}

TEST("the keys as picked share nothing; as they were, three keys ran two commands")
{
    CHECK(CountSharedChords(PICKED) == 0);
    for (std::size_t i = 0; i < PICKED.size(); ++i)
        CHECK(FindConflicts(PICKED, PICKED[i], i).empty());

    auto before = PICKED;
    before[GO_TO_AREA].chords = {KEY_M};
    before[PREVIOUS_AREA].contexts |= Bit(Context::Global);
    before[NEXT_AREA].contexts |= Bit(Context::Global);
    CHECK(CountSharedChords(before) == 3);
    CHECK(FindConflicts(before, before[ADD_MARKER], ADD_MARKER) == std::vector<std::size_t>{GO_TO_AREA});
    CHECK(FindConflicts(before, before[KEY_BEFORE], KEY_BEFORE) == std::vector<std::size_t>{PREVIOUS_AREA});
}

TEST("CO1: a key about to be set: the commands that have it where this one works")
{
    // Show the whole demo, set to G
    const Binding showAll = {{KEY_G}, Bit(Context::Global)};
    CHECK(FindConflicts(PICKED, showAll) == std::vector<std::size_t>{GO_TO_AREA});
    // with Shift, or where Go to Area doesn't work, G is free
    CHECK(FindConflicts(PICKED, {{SHIFT | KEY_G}, Bit(Context::Global)}).empty());
    CHECK(FindConflicts(PICKED, {{KEY_G}, Bit(Context::Placing)}).empty());
    // a command's own key isn't taken by itself
    CHECK(FindConflicts(PICKED, {{KEY_M}, EVERYWHERE}, ADD_MARKER).empty());
    // [ on another command that works anywhere: the key before, and while flying the area step
    CHECK(FindConflicts(PICKED, {{KEY_LEFT_BRACKET}, EVERYWHERE}) ==
          (std::vector<std::size_t>{PREVIOUS_AREA, KEY_BEFORE}));
}

TEST("keys used twice: each chord once, however many commands share it")
{
    const std::vector<Binding> bindings = {
        {{KEY_K}, Bit(Context::Global)},
        {{KEY_K}, Bit(Context::Global)},
        {{KEY_K, CTRL | KEY_D}, Bit(Context::Global)},
        // a held key and a chord of it, both while flying: Ctrl+D is shared, D alone isn't
        {{KEY_D}, Bit(Context::Fly), false, true},
        {{CTRL | KEY_D}, Bit(Context::Fly)},
        // the same key in contexts above and below each other is meant
        {{KEY_ESCAPE}, Bit(Context::Global)},
        {{KEY_ESCAPE}, Bit(Context::Placing)},
    };
    CHECK(CountSharedChords(bindings) == 2);
    CHECK(CountSharedChords({}) == 0);
}

TEST("which of a key's commands runs, in words")
{
    constexpr auto MAPS = "campaign and Zombies maps";
    const std::vector<ClashName> rebound = {{"Go to Area", MAPS}, {"Show the whole demo", "", true}};
    CHECK(DescribeClash(rebound) == "On campaign and Zombies maps Go to Area runs. On others this does.");
    const std::vector<ClashName> fromArea = {{"Go to Area", MAPS, true}, {"Show the whole demo"}};
    CHECK(DescribeClash(fromArea) == "On campaign and Zombies maps this runs. On others Show the whole demo does.");
    const std::vector<ClashName> anywhere = {{"Play"}, {"Add a node", "", true}};
    CHECK(DescribeClash(anywhere) == "Play runs. This only when it can't.");
    const std::vector<ClashName> first = {{"Add a node", "", true}, {"Play"}};
    CHECK(DescribeClash(first) == "This runs. Play only when it can't.");
    const std::vector<ClashName> three = {{"Play"}, {"Add a node", "", true}, {"Duplicate"}};
    CHECK(DescribeClash(three) == "They run in this order, each only when those before can't: Play, this, Duplicate.");
}
