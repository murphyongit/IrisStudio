#include "StdInclude.hpp"
#include "AreaLogic.hpp"

#include <glm/geometric.hpp>

namespace IWXMVM::Components::AreaLogic
{
    std::size_t Step(std::size_t current, std::size_t count, int by)
    {
        const auto size = static_cast<int>(count);
        return static_cast<std::size_t>(((static_cast<int>(current) + by) % size + size) % size);
    }

    std::size_t FindNearest(std::span<const Types::LevelArea> areas, const glm::vec3& position)
    {
        const auto nearest = std::ranges::min_element(
            areas, {}, [&](const Types::LevelArea& area) { return glm::distance(area.spot, position); });
        return static_cast<std::size_t>(nearest - areas.begin());
    }

    MapView FitMap(std::span<const glm::vec2> points, glm::vec2 size, float padding)
    {
        MapView view;
        view.size = size;
        if (points.empty())
            return view;
        auto mins = points.front(), maxs = points.front();
        for (const auto& point : points)
        {
            mins = glm::min(mins, point);
            maxs = glm::max(maxs, point);
        }
        view.middle = (mins + maxs) * 0.5f;
        const auto span = glm::max(maxs - mins, glm::vec2(1.0f));
        const auto room = glm::max(size - glm::vec2(padding * 2.0f), glm::vec2(1.0f));
        view.scale = std::min(room.x / span.x, room.y / span.y);
        return view;
    }

    glm::vec2 ToMap(const MapView& view, glm::vec2 point)
    {
        const auto offset = (point - view.middle) * view.scale;
        return view.size * 0.5f + glm::vec2(offset.x, -offset.y);
    }
}  // namespace IWXMVM::Components::AreaLogic
