#include "StdInclude.hpp"
#include "Markers.hpp"

#include "KeyframeSerializer.hpp"
#include "UndoHistory.hpp"

namespace IWXMVM::Components::Markers
{
    namespace
    {
        std::vector<Marker> markers;
        std::uint32_t nextId = 1;

        void Sort()
        {
            std::ranges::stable_sort(markers, {}, &Marker::tick);
        }

        // a change that was just made, from the markers as they were before, as a step to undo
        void Changed(std::vector<Marker> before, std::string label)
        {
            Sort();
            KeyframeSerializer::WriteRecentSoon();
            if (before == markers)
                return;
            UndoHistory::Get().Push(
                std::move(label),
                [before] {
                    markers = before;
                    KeyframeSerializer::WriteRecentSoon();
                },
                [after = markers] {
                    markers = after;
                    KeyframeSerializer::WriteRecentSoon();
                });
        }
    }  // namespace

    const std::vector<Marker>& GetAll()
    {
        return markers;
    }

    std::optional<Marker> Find(std::uint32_t id)
    {
        const auto it = std::ranges::find(markers, id, &Marker::id);
        return it != markers.end() ? std::optional(*it) : std::nullopt;
    }

    std::uint32_t Add(std::uint32_t tick, std::string name)
    {
        auto before = markers;
        const auto id = nextId++;
        if (name.empty())
            name = std::format("Marker {}", markers.size() + 1);
        markers.push_back({id, tick, std::move(name)});
        Changed(std::move(before), "add marker");
        return id;
    }

    void Remove(std::span<const std::uint32_t> ids, std::string label)
    {
        auto before = markers;
        std::erase_if(markers, [&](const Marker& marker) { return std::ranges::find(ids, marker.id) != ids.end(); });
        const auto removed = before.size() - markers.size();
        if (label.empty())
            label = removed == 1 ? "delete marker" : std::format("delete {} markers", removed);
        Changed(std::move(before), std::move(label));
    }

    void Rename(std::uint32_t id, std::string name)
    {
        auto before = markers;
        if (const auto it = std::ranges::find(markers, id, &Marker::id); it != markers.end())
            it->name = std::move(name);
        Changed(std::move(before), "rename marker");
    }

    void SetTick(std::uint32_t id, std::uint32_t tick)
    {
        if (const auto it = std::ranges::find(markers, id, &Marker::id); it != markers.end())
            it->tick = tick;
        Sort();
        KeyframeSerializer::WriteRecentSoon();
    }

    void PushMove(std::vector<Marker> before, std::string label)
    {
        Changed(std::move(before), std::move(label));
    }

    void Append(std::vector<Marker> added)
    {
        for (auto& marker : added)
        {
            marker.id = nextId++;
            markers.push_back(std::move(marker));
        }
        Sort();
    }

    void Clear()
    {
        markers.clear();
    }
}  // namespace IWXMVM::Components::Markers
