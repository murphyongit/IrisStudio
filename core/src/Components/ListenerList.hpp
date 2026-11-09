#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace IWXMVM::Components
{
    // Functions one thread calls while another may add to them: the mod's hooks go in on its own thread while the UI's
    // frames run. A call goes through a list that no longer changes, and an addition makes a new one, as a push_back under
    // a call moved the functions from under it. An addition shows from the next call on,
    // and a listener may add one.
    class ListenerList
    {
       public:
        void Add(std::function<void()> function)
        {
            std::lock_guard lock(addMutex);
            const auto current = list.load();
            auto added = current ? std::make_shared<List>(*current) : std::make_shared<List>();
            added->push_back(std::move(function));
            list.store(std::move(added));
        }

        void Call() const
        {
            if (const auto current = list.load())
                for (const auto& function : *current)
                    function();
        }

       private:
        using List = std::vector<std::function<void()>>;
        std::atomic<std::shared_ptr<const List>> list;
        std::mutex addMutex;
    };
}  // namespace IWXMVM::Components
