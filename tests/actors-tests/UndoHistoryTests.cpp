#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/UndoHistory.hpp"

using IWXMVM::Components::UndoHistory;

TEST("undo takes back the latest step, wherever it was")
{
    UndoHistory history;
    int keys = 0, actors = 0;
    history.Push("add node", [&] { --keys; }, [&] { ++keys; });
    ++keys;
    history.Push("add actor", [&] { --actors; }, [&] { ++actors; });
    ++actors;

    CHECK(history.GetUndoLabel() == "add actor");
    CHECK(history.Undo() == "add actor");
    CHECK(actors == 0);
    CHECK(keys == 1);
    CHECK(history.Undo() == "add node");
    CHECK(keys == 0);
    CHECK(!history.Undo());

    CHECK(history.GetRedoLabel() == "add node");
    CHECK(history.Redo() == "add node");
    CHECK(history.Redo() == "add actor");
    CHECK(keys == 1);
    CHECK(actors == 1);
    CHECK(!history.Redo());
}

TEST("a new step forgets what was undone")
{
    UndoHistory history;
    history.Push("a", [] {}, [] {});
    history.Undo();
    CHECK(history.GetRedoLabel() == "a");
    history.Push("b", [] {}, [] {});
    CHECK(!history.GetRedoLabel());
    CHECK(history.GetUndoLabel() == "b");
}

TEST("a group is one step, undone backwards")
{
    UndoHistory history;
    std::string order;
    history.BeginGroup("delete 3 keys");
    history.Push("delete key", [&] { order += "a"; }, [&] { order += "A"; });
    history.BeginGroup("inner");
    history.Push("delete key", [&] { order += "b"; }, [&] { order += "B"; });
    history.EndGroup();
    history.Push("delete key", [&] { order += "c"; }, [&] { order += "C"; });
    // nothing to undo while the group is open
    CHECK(!history.Undo());
    history.EndGroup();

    CHECK(history.GetUndoLabel() == "delete 3 keys");
    CHECK(history.Undo() == "delete 3 keys");
    CHECK(order == "cba");
    CHECK(!history.Undo());
    history.Redo();
    CHECK(order == "cbaABC");

    // an empty group adds nothing
    history.BeginGroup("nothing");
    history.EndGroup();
    CHECK(history.GetUndoLabel() == "delete 3 keys");
}

TEST("the latest step's id tells whether a part's step is still on top")
{
    UndoHistory history;
    CHECK(history.GetLatest() == 0);
    const auto actor = history.Push("move actor", [] {}, [] {});
    CHECK(history.GetLatest() == actor);
    const auto key = history.Push("add key", [] {}, [] {});
    CHECK(history.GetLatest() == key);
    CHECK(key != actor);
    history.Undo();
    CHECK(history.GetLatest() == actor);
}

TEST("while a group is open its step is the latest, so a part's step started in it stays on top of it")
{
    // a drag that moves keys and an actor's clip is one step: the actors' step begins with the first change and goes
    // on while the latest step is still the one it pushed
    UndoHistory history;
    history.Push("before", [] {}, [] {});
    history.BeginGroup("move 3 items");
    const auto group = history.GetLatest();
    const auto actorStep = history.Push("change Actor 1", [] {}, [] {});
    CHECK(actorStep == group);
    CHECK(history.GetLatest() == actorStep);
    history.Push("move key", [] {}, [] {});
    CHECK(history.GetLatest() == actorStep);
    history.EndGroup();
    CHECK(history.GetLatest() == actorStep);
    CHECK(history.GetUndoLabel() == "move 3 items");
}

TEST("the history keeps the latest steps")
{
    UndoHistory history;
    int undone = 0;
    for (std::size_t i = 0; i < UndoHistory::MAX_STEPS + 10; ++i)
        history.Push(std::to_string(i), [&] { ++undone; }, [] {});
    while (history.Undo())
        ;
    CHECK(undone == static_cast<int>(UndoHistory::MAX_STEPS));
    history.Clear();
    CHECK(!history.GetRedoLabel());
}
