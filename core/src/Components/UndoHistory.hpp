#pragma once
#include <deque>
#include <mutex>
#include <optional>

namespace IWXMVM::Components
{
    // One history of what the user changed, whichever part of the editor it was in (keyframes, actors, the capture
    // range...): undo takes back the latest step wherever it was. Each part pushes a step as it changes something,
    // with how to undo and redo it. Steps can be pushed from any thread; undo and redo run on the caller's.
    class UndoHistory
    {
       public:
        // the editor's
        static UndoHistory& Get()
        {
            static UndoHistory instance;
            return instance;
        }

        static constexpr std::size_t MAX_STEPS = 200;

        // Adds a step that was just done, named for the Edit menu ("move 3 keys"), and forgets what was undone.
        // Returns its id.
        std::uint64_t Push(std::string label, std::function<void()> undo, std::function<void()> redo);

        // The steps pushed until the matching EndGroup are one, with this label (the outermost group's)
        void BeginGroup(std::string label);
        void EndGroup();

        // Undoes (redoes) the latest step and returns its label, nothing when there is none
        std::optional<std::string> Undo();
        std::optional<std::string> Redo();
        std::optional<std::string> GetUndoLabel() const;
        std::optional<std::string> GetRedoLabel() const;

        // the id of the step Undo would take back, 0 when there is none; while a group is open, the group's (the step
        // it becomes)
        std::uint64_t GetLatest() const;

        void Clear();

       private:
        struct Step
        {
            std::uint64_t id = 0;
            std::string label;
            std::vector<std::function<void()>> undo;  // in the order they were pushed: run backwards
            std::vector<std::function<void()>> redo;
        };

        mutable std::mutex mutex;
        std::deque<Step> done;
        std::vector<Step> undone;
        std::uint64_t nextId = 1;

        std::uint32_t groupDepth = 0;
        std::optional<Step> group;
    };
}  // namespace IWXMVM::Components
