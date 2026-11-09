#include "StdInclude.hpp"
#include "UndoHistory.hpp"

namespace IWXMVM::Components
{
    std::uint64_t UndoHistory::Push(std::string label, std::function<void()> undo, std::function<void()> redo)
    {
        std::lock_guard lock(mutex);
        undone.clear();
        if (group)
        {
            group->undo.push_back(std::move(undo));
            group->redo.push_back(std::move(redo));
            return group->id;
        }
        done.push_back({nextId++, std::move(label), {std::move(undo)}, {std::move(redo)}});
        if (done.size() > MAX_STEPS)
            done.pop_front();
        return done.back().id;
    }

    void UndoHistory::BeginGroup(std::string label)
    {
        std::lock_guard lock(mutex);
        if (groupDepth++ == 0)
            group = Step{nextId++, std::move(label)};
    }

    void UndoHistory::EndGroup()
    {
        std::lock_guard lock(mutex);
        if (groupDepth == 0 || --groupDepth > 0)
            return;
        if (!group->undo.empty())
        {
            done.push_back(std::move(*group));
            if (done.size() > MAX_STEPS)
                done.pop_front();
        }
        group.reset();
    }

    std::optional<std::string> UndoHistory::Undo()
    {
        Step step;
        {
            std::lock_guard lock(mutex);
            if (done.empty() || group)
                return std::nullopt;
            step = std::move(done.back());
            done.pop_back();
        }
        // (unlocked: undoing takes the locks of what it changes, which may be pushing a step meanwhile)
        for (auto it = step.undo.rbegin(); it != step.undo.rend(); ++it)
            (*it)();
        auto label = step.label;
        std::lock_guard lock(mutex);
        undone.push_back(std::move(step));
        return label;
    }

    std::optional<std::string> UndoHistory::Redo()
    {
        Step step;
        {
            std::lock_guard lock(mutex);
            if (undone.empty() || group)
                return std::nullopt;
            step = std::move(undone.back());
            undone.pop_back();
        }
        for (const auto& redo : step.redo)
            redo();
        auto label = step.label;
        std::lock_guard lock(mutex);
        done.push_back(std::move(step));
        return label;
    }

    std::optional<std::string> UndoHistory::GetUndoLabel() const
    {
        std::lock_guard lock(mutex);
        return done.empty() ? std::nullopt : std::optional(done.back().label);
    }

    std::optional<std::string> UndoHistory::GetRedoLabel() const
    {
        std::lock_guard lock(mutex);
        return undone.empty() ? std::nullopt : std::optional(undone.back().label);
    }

    std::uint64_t UndoHistory::GetLatest() const
    {
        std::lock_guard lock(mutex);
        // (an open group is the step being made: what is pushed meanwhile goes into it)
        if (group)
            return group->id;
        return done.empty() ? 0 : done.back().id;
    }

    void UndoHistory::Clear()
    {
        std::lock_guard lock(mutex);
        done.clear();
        undone.clear();
    }
}  // namespace IWXMVM::Components
