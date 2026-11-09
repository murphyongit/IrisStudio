#pragma once
#include "UI/CommandDispatch.hpp"

// The editor's commands: every action with a key has one name and one shortcut, shown in the Controls window, menus,
// tooltips and the shortcuts sheet, and rebindable there. Keys reach the commands through input contexts (see
// CommandDispatch): flying, typing or a popup keep keys from the editor's shortcuts, a chord matches its modifiers
// exactly and held keys only repeat a command that asks for it.
namespace IWXMVM::UI::Commands
{
    using Context = CommandDispatch::Context;
    using ContextMask = CommandDispatch::ContextMask;
    using CommandDispatch::Bit;

    struct Command
    {
        std::string_view id;        // "edit.duplicate", what config.json keeps the user's keys by
        std::string_view label;     // "Duplicate"
        std::string_view category;  // "Edit", for the Controls window and the shortcuts sheet
        std::vector<ImGuiKeyChord> defaults;
        ContextMask contexts = Bit(Context::Global);
        // a held press runs it again at the key repeat rate (skips); else once a press
        bool repeat = false;
        // a key to hold (fly forward), asked for with IsHeld: matched with any modifiers and never run
        bool held = false;
        // why it can't run now, for its tooltip; nothing when it can
        std::function<std::optional<std::string>()> unavailable;
        std::function<void()> run;
        // other words for it, for finding it by name ("dolly path" for the campath)
        std::string_view keywords;
        // the maps it works on when that isn't all of them ("campaign and Zombies maps"), for saying which of a key's
        // commands runs
        std::string_view maps;
    };

    // adds a command, or replaces the one with its id
    void Register(Command command);

    // Runs the commands the keys pressed this frame ask for, in the contexts active this frame; once a frame, before
    // the UI draws
    void Dispatch(ContextMask active);
    ContextMask GetActiveContexts();
    // whether a context is active this frame, e.g. the mouse over the timeline
    bool IsActive(Context context);

    // whether a held command's key is down, while one of its contexts takes keys
    bool IsHeld(std::string_view id);

    // Whether a press of the key with the modifiers would be the editor's, as Dispatch would have it in the contexts of
    // this frame: one a command runs for or holds, for keeping it from the game's binds (InputRouting)
    bool UsesPress(ImGuiKey key, ImGuiKeyChord modifiers, bool repeat);

    // Runs a command as its key would (a menu item, a button), if it is available; returns whether it ran
    bool Run(std::string_view id);
    std::optional<std::string> GetUnavailableReason(std::string_view id);

    const std::vector<Command>& GetAll();
    const Command* Find(std::string_view id);

    // the chords a command has now: the user's, else its defaults
    std::vector<ImGuiKeyChord> GetChords(std::string_view id);
    // the user's chords for a command (saved in config.json), or nothing for its defaults
    void SetChords(std::string_view id, std::optional<std::vector<ImGuiKeyChord>> chords);
    bool IsCustomized(std::string_view id);
    void ResetAll();

    // the commands a press of one of this one's chords could also run (see CommandDispatch::Conflict), in the order a
    // press tries them; with chords, as they would be with those (a key about to be set)
    std::vector<const Command*> GetConflicts(std::string_view id);
    std::vector<const Command*> GetConflicts(std::string_view id, const std::vector<ImGuiKeyChord>& chords);
    // how many keys more than one command has (each counted once)
    std::size_t CountSharedKeys();
    // which of a command and the others its chord would also run does run, in words (CommandDispatch::DescribeClash)
    std::string DescribeClash(std::string_view id, const std::vector<const Command*>& others);

    // a chord as it is shown, "Ctrl+D", "?", "Right mouse"
    std::string FormatChord(ImGuiKeyChord chord);
    // a command's first chord as it is shown, empty without one
    std::string GetShortcut(std::string_view id);
    // the names of contexts a command works in, "while flying, anywhere"
    std::string DescribeContexts(ContextMask contexts);

    // whether a command matches what was typed to find it: every word in its label, category, id, other words for
    // it or keys, whatever the case
    bool MatchesSearch(const Command& command, std::string_view search);
}  // namespace IWXMVM::UI::Commands
