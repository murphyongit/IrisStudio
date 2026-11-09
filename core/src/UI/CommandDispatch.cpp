#include "StdInclude.hpp"
#include "CommandDispatch.hpp"

namespace IWXMVM::UI::CommandDispatch
{
    bool TakesEveryKey(Context context)
    {
        return context == Context::Typing || context == Context::Popup || context == Context::Capturing ||
               context == Context::Fly;
    }

    ContextMask Reachable(ContextMask active)
    {
        ContextMask reachable = 0;
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(Context::Count); ++i)
        {
            const auto context = static_cast<Context>(i);
            if (!(active & Bit(context)))
                continue;
            reachable |= Bit(context);
            if (TakesEveryKey(context))
                break;
        }
        return reachable;
    }

    bool Matches(const Binding& binding, std::int32_t key, Chord modifiers)
    {
        for (const auto chord : binding.chords)
        {
            if (KeyOf(chord) == 0 || KeyOf(chord) != key)
                continue;
            if (binding.held || ModifiersOf(chord) == ModifiersOf(modifiers))
                return true;
        }
        return false;
    }

    Outcome Dispatch(std::span<const Binding> bindings, Press press, Chord modifiers, ContextMask active,
                     const std::function<bool(std::size_t)>& available)
    {
        const auto reachable = Reachable(active);
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(Context::Count); ++i)
        {
            const auto context = Bit(static_cast<Context>(i));
            if (!(reachable & context))
                continue;
            for (std::size_t index = 0; index < bindings.size(); ++index)
            {
                const auto& binding = bindings[index];
                if (!(binding.contexts & context) || !Matches(binding, press.key, modifiers) || !available(index))
                    continue;
                if (binding.held || (press.repeat && !binding.repeat))
                    return {std::nullopt, true};
                return {index, true};
            }
        }
        return {};
    }

    bool IsHeld(const Binding& binding, ContextMask active, const std::function<bool(std::int32_t key)>& keyDown)
    {
        if (!binding.held || !(binding.contexts & Reachable(active)))
            return false;
        return std::ranges::any_of(binding.chords,
                                   [&](Chord chord) { return KeyOf(chord) != 0 && keyDown(KeyOf(chord)); });
    }

    bool Conflict(const Binding& a, const Binding& b)
    {
        if (!(a.contexts & b.contexts))
            return false;
        for (const auto chord : a.chords)
        {
            if (KeyOf(chord) != 0 && Matches(b, KeyOf(chord), ModifiersOf(chord)))
                return true;
        }
        // (a held key matches b's chords with any modifiers)
        for (const auto chord : b.chords)
        {
            if (KeyOf(chord) != 0 && Matches(a, KeyOf(chord), ModifiersOf(chord)))
                return true;
        }
        return false;
    }

    std::vector<std::size_t> FindConflicts(std::span<const Binding> bindings, const Binding& candidate,
                                           std::optional<std::size_t> skip)
    {
        std::vector<std::size_t> conflicts;
        for (std::size_t i = 0; i < bindings.size(); ++i)
        {
            if (i != skip && Conflict(candidate, bindings[i]))
                conflicts.push_back(i);
        }
        return conflicts;
    }

    std::size_t CountSharedChords(std::span<const Binding> bindings)
    {
        std::vector<Chord> shared;
        for (std::size_t i = 0; i < bindings.size(); ++i)
        {
            for (const auto chord : bindings[i].chords)
            {
                if (KeyOf(chord) == 0 || std::ranges::find(shared, chord) != shared.end())
                    continue;
                // a press of the chord another binding takes too (a held one with any modifiers: a held D and a
                // Ctrl+D are one key used twice, Ctrl+D)
                for (std::size_t j = 0; j < bindings.size(); ++j)
                {
                    if (j != i && (bindings[i].contexts & bindings[j].contexts) &&
                        Matches(bindings[j], KeyOf(chord), ModifiersOf(chord)))
                    {
                        shared.push_back(chord);
                        break;
                    }
                }
            }
        }
        return shared.size();
    }

    std::string DescribeClash(std::span<const ClashName> names)
    {
        const auto name = [](const ClashName& entry, bool start) {
            if (!entry.self)
                return std::string(entry.label);
            return std::string(start ? "This" : "this");
        };
        if (names.size() == 2)
        {
            const auto& first = names[0];
            const auto& second = names[1];
            if (!first.maps.empty())
                return std::format("On {} {} runs. On others {} does.", first.maps, name(first, false),
                                   name(second, false));
            return std::format("{} runs. {} only when it can't.", name(first, true), name(second, true));
        }
        std::string order;
        for (const auto& entry : names)
            order += (order.empty() ? "" : ", ") + name(entry, false);
        return std::format("They run in this order, each only when those before can't: {}.", order);
    }
}  // namespace IWXMVM::UI::CommandDispatch
