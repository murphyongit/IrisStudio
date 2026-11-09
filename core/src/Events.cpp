#include "StdInclude.hpp"
#include "Events.hpp"

#include "Components/ListenerList.hpp"

namespace IWXMVM::Events
{
    // (a table, as a map changes as an event is first asked for, under the frames too)
    std::array<Components::ListenerList, static_cast<std::size_t>(EventType::OnRewindCompleted) + 1> listeners;

    void Invoke(const EventType eventType)
    {
        listeners[static_cast<std::size_t>(eventType)].Call();
    }

    void RegisterListener(const EventType eventType, const std::function<void()> function)
    {
        listeners[static_cast<std::size_t>(eventType)].Add(function);
    }
}  // namespace IWXMVM::Events