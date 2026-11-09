#include "StdInclude.hpp"
#include "ViewLines.hpp"

namespace IWXMVM::GFX::ViewLines
{
    namespace
    {
        // the frame's width and height for a size in front of the node: the FOV's, a little under it to keep it small, at
        // the picture's shape
        constexpr float FRAME_WIDTH = 0.72f;
        constexpr float FRAME_ASPECT = 9.0f / 16.0f;
        // the mark over the frame, by the frame's half width and half height
        constexpr float MARK_WIDTH = 0.45f;
        constexpr float MARK_BASE = 1.12f;
        constexpr float MARK_TOP = 1.65f;
        constexpr int RING_POINTS = 20;

        // the parts of a line ahead of the eye, each its points in order
        std::vector<std::vector<glm::vec3>> CutAtEye(const Eye& eye, std::span<const glm::vec3> line)
        {
            std::vector<std::vector<glm::vec3>> parts;
            std::vector<glm::vec3> part;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                const auto depth = GetDepth(eye, line[i]);
                if (i > 0)
                {
                    const auto before = GetDepth(eye, line[i - 1]);
                    // (where it crosses)
                    if ((before >= NEAREST) != (depth >= NEAREST))
                        part.push_back(glm::mix(line[i - 1], line[i], (NEAREST - before) / (depth - before)));
                    if (depth < NEAREST && !part.empty())
                        parts.push_back(std::exchange(part, {}));
                }
                if (depth >= NEAREST)
                    part.push_back(line[i]);
            }
            if (!part.empty())
                parts.push_back(std::move(part));
            return parts;
        }
    }  // namespace

    Eye MakeEye(glm::vec3 position, glm::vec3 forward, glm::vec3 up, float fov, float viewWidth)
    {
        return {position, forward, up, viewWidth / 2.0f / std::tan(glm::radians(fov) / 2.0f)};
    }

    float GetDepth(const Eye& eye, glm::vec3 point)
    {
        return glm::dot(point - eye.position, eye.forward);
    }

    float GetUnitsPerPixel(const Eye& eye, float depth)
    {
        return std::max(depth, NEAREST) / eye.pixelsAtOne;
    }

    std::vector<Strip> GetStrips(const Eye& eye, std::span<const glm::vec3> line, float widthPixels)
    {
        std::vector<Strip> strips;
        for (const auto& part : CutAtEye(eye, line))
        {
            if (part.size() < 2)
                continue;
            // (a ring closes on itself: its first and last points have the same neighbours)
            const bool closed = part.front() == part.back() && part.size() > 2;
            Strip strip;
            auto side = glm::vec3(0.0f);
            for (std::size_t i = 0; i < part.size(); ++i)
            {
                const auto last = part.size() - 1;
                const auto before = i > 0 ? part[i - 1] : closed ? part[last - 1] : part[0];
                const auto after = i < last ? part[i + 1] : closed ? part[1] : part[last];
                const auto across = glm::cross(after - before, eye.position - part[i]);
                // (a line running straight at the eye has no side: it keeps the last it had, or the eye's up)
                if (glm::length(across) > 1e-5f)
                    side = glm::normalize(across);
                else if (glm::length(side) == 0.0f)
                    side = eye.up;
                const auto half = GetUnitsPerPixel(eye, GetDepth(eye, part[i])) * widthPixels / 2.0f;
                strip.left.push_back(part[i] - side * half);
                strip.right.push_back(part[i] + side * half);
            }
            strips.push_back(std::move(strip));
        }
        return strips;
    }

    float GetNodeSize(const Eye& eye, glm::vec3 position)
    {
        const auto pixels = NODE_UNITS / GetUnitsPerPixel(eye, GetDepth(eye, position));
        return pixels >= NODE_LEAST_PIXELS ? NODE_UNITS : NODE_UNITS * NODE_LEAST_PIXELS / pixels;
    }

    NodeShape GetNodeShape(glm::vec3 position, glm::vec3 forward, glm::vec3 right, glm::vec3 up, float fov, float size)
    {
        const auto halfWidth = size * std::tan(glm::radians(fov) / 2.0f) * FRAME_WIDTH;
        const auto halfHeight = halfWidth * FRAME_ASPECT;
        const auto at = [&](float across, float upward) {
            return position + forward * size + right * across + up * upward;
        };
        const std::array corners{at(-halfWidth, -halfHeight), at(halfWidth, -halfHeight), at(halfWidth, halfHeight),
                                 at(-halfWidth, halfHeight)};
        NodeShape shape;
        shape.lines.push_back({corners[0], corners[1], corners[2], corners[3], corners[0]});
        for (const auto& corner : corners)
            shape.lines.push_back({position, corner});
        shape.mark = {at(-halfWidth * MARK_WIDTH, halfHeight * MARK_BASE), at(halfWidth * MARK_WIDTH, halfHeight * MARK_BASE),
                      at(0.0f, halfHeight * MARK_TOP)};
        return shape;
    }

    std::vector<glm::vec3> GetRing(const Eye& eye, glm::vec3 center, float radiusPixels)
    {
        const auto depth = GetDepth(eye, center);
        if (depth < NEAREST)
            return {};
        const auto radius = GetUnitsPerPixel(eye, depth) * radiusPixels;
        const auto right = glm::normalize(glm::cross(eye.forward, eye.up));
        std::vector<glm::vec3> ring;
        for (int i = 0; i <= RING_POINTS; ++i)
        {
            const auto angle = glm::two_pi<float>() * (i % RING_POINTS) / RING_POINTS;
            ring.push_back(center + (right * std::cos(angle) + eye.up * std::sin(angle)) * radius);
        }
        return ring;
    }

    std::optional<std::array<glm::vec3, 3>> GetArrowHead(const Eye& eye, glm::vec3 from, glm::vec3 tip)
    {
        const auto depth = GetDepth(eye, tip);
        if (depth < NEAREST || tip == from)
            return std::nullopt;
        const auto units = GetUnitsPerPixel(eye, depth);
        const auto along = glm::normalize(tip - from);
        const auto across = glm::cross(along, eye.position - tip);
        const auto side = glm::length(across) > 1e-5f ? glm::normalize(across) : eye.up;
        const auto base = tip - along * (HEAD_LENGTH_PIXELS * units);
        return std::array{base - side * (HEAD_WIDTH_PIXELS * units), base + side * (HEAD_WIDTH_PIXELS * units), tip};
    }

    float StepTowards(float value, float target, float step)
    {
        return value < target ? std::min(value + step, target) : std::max(value - step, target);
    }
}  // namespace IWXMVM::GFX::ViewLines
