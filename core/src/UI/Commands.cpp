#include "StdInclude.hpp"
#include "Commands.hpp"

#include "Configuration/InputConfiguration.hpp"

namespace IWXMVM::UI::Commands
{
    static_assert(CommandDispatch::CTRL == ImGuiMod_Ctrl && CommandDispatch::SHIFT == ImGuiMod_Shift &&
                  CommandDispatch::ALT == ImGuiMod_Alt && CommandDispatch::SUPER == ImGuiMod_Super);

    namespace
    {
        // (all on the render thread, which draws the UI)
        std::vector<Command> commands;
        std::vector<CommandDispatch::Binding> bindings;
        bool bindingsChanged = true;
        ContextMask active = Bit(Context::Global);

        const CommandDispatch::Binding* FindBinding(std::string_view id)
        {
            for (std::size_t i = 0; i < commands.size(); ++i)
            {
                if (commands[i].id == id)
                    return &bindings[i];
            }
            return nullptr;
        }

        void UpdateBindings()
        {
            if (!bindingsChanged)
                return;
            bindingsChanged = false;
            bindings.clear();
            for (const auto& command : commands)
                bindings.push_back({GetChords(command.id), command.contexts, command.repeat, command.held});
        }

        bool IsAvailable(const Command& command)
        {
            return !command.unavailable || !command.unavailable();
        }

        // The keys a press can be: not the mouse (clicks are the UI's) nor the modifiers' own entries
        bool IsPressableKey(ImGuiKey key)
        {
            if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY)
                return false;
            if (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown)
                return false;
            return key < ImGuiKey_ReservedForModCtrl || key > ImGuiKey_ReservedForModSuper;
        }

        std::string KeyName(ImGuiKey key)
        {
            switch (key)
            {
                case ImGuiKey_None:
                    return "";
                case ImGuiKey_LeftCtrl:
                case ImGuiKey_RightCtrl:
                    return "Ctrl";
                case ImGuiKey_LeftShift:
                case ImGuiKey_RightShift:
                    return "Shift";
                case ImGuiKey_LeftAlt:
                case ImGuiKey_RightAlt:
                    return "Alt";
                case ImGuiKey_LeftArrow:
                    return "Left";
                case ImGuiKey_RightArrow:
                    return "Right";
                case ImGuiKey_UpArrow:
                    return "Up";
                case ImGuiKey_DownArrow:
                    return "Down";
                case ImGuiKey_Escape:
                    return "Esc";
                case ImGuiKey_Comma:
                    return ",";
                case ImGuiKey_Period:
                    return ".";
                case ImGuiKey_Slash:
                    return "/";
                case ImGuiKey_Semicolon:
                    return ";";
                case ImGuiKey_Apostrophe:
                    return "'";
                case ImGuiKey_LeftBracket:
                    return "[";
                case ImGuiKey_RightBracket:
                    return "]";
                case ImGuiKey_Backslash:
                    return "\\";
                case ImGuiKey_Minus:
                    return "-";
                case ImGuiKey_Equal:
                    return "=";
                case ImGuiKey_GraveAccent:
                    return "`";
                case ImGuiKey_PageUp:
                    return "Page Up";
                case ImGuiKey_PageDown:
                    return "Page Down";
                case ImGuiKey_MouseLeft:
                    return "Left mouse";
                case ImGuiKey_MouseRight:
                    return "Right mouse";
                case ImGuiKey_MouseMiddle:
                    return "Middle mouse";
                case ImGuiKey_MouseX1:
                    return "Mouse 4";
                case ImGuiKey_MouseX2:
                    return "Mouse 5";
                default:
                    return ImGui::GetKeyName(key);
            }
        }
    }  // namespace

    void Register(Command command)
    {
        bindingsChanged = true;
        for (auto& existing : commands)
        {
            if (existing.id == command.id)
            {
                existing = std::move(command);
                return;
            }
        }
        commands.push_back(std::move(command));
    }

    void Dispatch(ContextMask contexts)
    {
        active = contexts;
        UpdateBindings();

        const auto modifiers = ImGui::GetIO().KeyMods;
        for (auto key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key = static_cast<ImGuiKey>(key + 1))
        {
            if (!IsPressableKey(key))
                continue;
            const bool pressed = ImGui::IsKeyPressed(key, false);
            const bool repeated = !pressed && ImGui::IsKeyPressed(key, true);
            if (!pressed && !repeated)
                continue;
            const auto outcome = CommandDispatch::Dispatch(
                bindings, {key, repeated}, modifiers, active,
                [](std::size_t index) { return IsAvailable(commands[index]); });
            if (outcome.run)
            {
                // (a command may register others)
                const auto run = commands[*outcome.run].run;
                if (run)
                    run();
                UpdateBindings();
            }
        }
    }

    ContextMask GetActiveContexts()
    {
        return active;
    }

    bool IsActive(Context context)
    {
        return active & Bit(context);
    }

    bool IsHeld(std::string_view id)
    {
        UpdateBindings();
        const auto binding = FindBinding(id);
        const auto command = Find(id);
        if (!binding || !command)
            return false;
        return CommandDispatch::IsHeld(*binding, active,
                                       [](std::int32_t key) { return ImGui::IsKeyDown(static_cast<ImGuiKey>(key)); }) &&
               IsAvailable(*command);
    }

    bool UsesPress(ImGuiKey key, ImGuiKeyChord modifiers, bool repeat)
    {
        if (!IsPressableKey(key))
            return false;
        UpdateBindings();
        return CommandDispatch::Dispatch(bindings, {key, repeat}, modifiers, active,
                                         [](std::size_t index) { return IsAvailable(commands[index]); })
            .used;
    }

    bool Run(std::string_view id)
    {
        const auto command = Find(id);
        if (!command || !command->run || !IsAvailable(*command))
            return false;
        const auto run = command->run;
        run();
        return true;
    }

    std::optional<std::string> GetUnavailableReason(std::string_view id)
    {
        const auto command = Find(id);
        if (!command)
            return "There is no such command";
        return command->unavailable ? command->unavailable() : std::nullopt;
    }

    const std::vector<Command>& GetAll()
    {
        return commands;
    }

    const Command* Find(std::string_view id)
    {
        const auto it = std::ranges::find(commands, id, &Command::id);
        return it != commands.end() ? &*it : nullptr;
    }

    std::vector<ImGuiKeyChord> GetChords(std::string_view id)
    {
        if (auto chords = InputConfiguration::Get().GetChords(id))
            return *chords;
        const auto command = Find(id);
        return command ? command->defaults : std::vector<ImGuiKeyChord>{};
    }

    void SetChords(std::string_view id, std::optional<std::vector<ImGuiKeyChord>> chords)
    {
        // the defaults again are no change of the user's
        const auto command = Find(id);
        if (chords && command && *chords == command->defaults)
            chords.reset();
        InputConfiguration::Get().SetChords(id, std::move(chords));
        bindingsChanged = true;
    }

    bool IsCustomized(std::string_view id)
    {
        return InputConfiguration::Get().GetChords(id).has_value();
    }

    void ResetAll()
    {
        InputConfiguration::Get().ResetAll();
        bindingsChanged = true;
    }

    std::vector<const Command*> GetConflicts(std::string_view id)
    {
        return GetConflicts(id, GetChords(id));
    }

    std::vector<const Command*> GetConflicts(std::string_view id, const std::vector<ImGuiKeyChord>& chords)
    {
        UpdateBindings();
        std::vector<const Command*> conflicts;
        const auto it = std::ranges::find(commands, id, &Command::id);
        if (it == commands.end())
            return conflicts;
        const auto index = static_cast<std::size_t>(it - commands.begin());
        auto candidate = bindings[index];
        candidate.chords = chords;
        for (const auto i : CommandDispatch::FindConflicts(bindings, candidate, index))
            conflicts.push_back(&commands[i]);
        return conflicts;
    }

    std::size_t CountSharedKeys()
    {
        UpdateBindings();
        return CommandDispatch::CountSharedChords(bindings);
    }

    std::string DescribeClash(std::string_view id, const std::vector<const Command*>& others)
    {
        const auto self = Find(id);
        if (!self)
            return "";
        // in the order of the commands, as a press tries them
        std::vector<const Command*> all = others;
        all.push_back(self);
        std::ranges::sort(all);
        std::vector<CommandDispatch::ClashName> names;
        for (const auto command : all)
            names.push_back({command->label, command->maps, command == self});
        return CommandDispatch::DescribeClash(names);
    }

    std::string FormatChord(ImGuiKeyChord chord)
    {
        const auto key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
        const auto modifiers = chord & ImGuiMod_Mask_;
        if (key == ImGuiKey_None)
            return "";
        // (Shift+/ on most keyboards)
        if (key == ImGuiKey_Slash && modifiers == ImGuiMod_Shift)
            return "?";

        std::string text;
        if (modifiers & ImGuiMod_Ctrl)
            text += "Ctrl+";
        if (modifiers & ImGuiMod_Shift)
            text += "Shift+";
        if (modifiers & ImGuiMod_Alt)
            text += "Alt+";
        if (modifiers & ImGuiMod_Super)
            text += "Win+";
        return text + KeyName(key);
    }

    std::string GetShortcut(std::string_view id)
    {
        const auto chords = GetChords(id);
        return chords.empty() ? std::string() : FormatChord(chords.front());
    }

    std::string DescribeContexts(ContextMask contexts)
    {
        static constexpr std::array<std::pair<Context, std::string_view>, 9> NAMES = {{
            {Context::Typing, "typing"},
            {Context::Popup, "in a popup"},
            {Context::Capturing, "while recording"},
            {Context::Fly, "while flying"},
            {Context::Placing, "while placing"},
            {Context::Viewport, "over the view"},
            {Context::Timeline, "over the timeline"},
            {Context::Panel, "in the side panel"},
            {Context::Global, "anywhere"},
        }};
        std::string text;
        for (const auto& [context, name] : NAMES)
        {
            if (!(contexts & Bit(context)))
                continue;
            text += text.empty() ? "" : ", ";
            text += name;
        }
        return text;
    }

    bool MatchesSearch(const Command& command, std::string_view search)
    {
        const auto lower = [](std::string_view text) {
            std::string result(text);
            std::ranges::transform(result, result.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        };
        auto haystack = lower(std::format("{} {} {} {}", command.label, command.category, command.id, command.keywords));
        for (const auto chord : GetChords(command.id))
            haystack += " " + lower(FormatChord(chord));

        std::size_t start = 0;
        const auto needle = lower(search);
        while (start < needle.size())
        {
            const auto end = std::min(needle.find(' ', start), needle.size());
            if (end > start && haystack.find(needle.substr(start, end - start)) == std::string::npos)
                return false;
            start = end + 1;
        }
        return true;
    }
}  // namespace IWXMVM::UI::Commands
