#include "StdInclude.hpp"
#include "ShotLogic.hpp"

#include <glm/gtc/quaternion.hpp>

namespace IWXMVM::Components::ShotLogic
{
    namespace
    {
        glm::vec3 ClosestOnSegment(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b)
        {
            const auto axis = b - a;
            const auto squared = glm::dot(axis, axis);
            if (squared <= 0.0f)
                return a;
            return a + axis * std::clamp(glm::dot(point - a, axis) / squared, 0.0f, 1.0f);
        }

        // the nearer of a line's two meetings with a sphere, ahead of its start
        std::optional<float> HitSphere(const glm::vec3& start, const glm::vec3& direction, const glm::vec3& center,
                                       float radius)
        {
            const auto offset = start - center;
            const auto along = glm::dot(offset, direction);
            const auto discriminant = along * along - (glm::dot(offset, offset) - radius * radius);
            if (discriminant < 0.0f)
                return std::nullopt;
            const auto distance = -along - std::sqrt(discriminant);
            return distance >= 0.0f ? std::optional(distance) : std::nullopt;
        }
    }  // namespace

    std::optional<float> HitCapsule(const glm::vec3& start, const glm::vec3& direction, float length, const glm::vec3& a,
                                    const glm::vec3& b, float radius)
    {
        if (radius <= 0.0f || glm::distance(start, ClosestOnSegment(start, a, b)) <= radius)
            return std::nullopt;

        std::optional<float> best;
        const auto keep = [&](std::optional<float> distance) {
            if (distance && (!best || *distance < *best))
                best = distance;
        };
        keep(HitSphere(start, direction, a, radius));
        keep(HitSphere(start, direction, b, radius));

        // the side, by the line's distance from the axis
        const auto axis = b - a;
        const auto squared = glm::dot(axis, axis);
        const auto offset = start - a;
        const auto offsetAlong = glm::dot(offset, axis), directionAlong = glm::dot(direction, axis);
        const auto quadratic = squared - directionAlong * directionAlong;
        constexpr float PARALLEL = 0.0001f;
        if (quadratic > PARALLEL * squared)
        {
            const auto linear = squared * glm::dot(offset, direction) - directionAlong * offsetAlong;
            const auto constant =
                squared * (glm::dot(offset, offset) - radius * radius) - offsetAlong * offsetAlong;
            const auto discriminant = linear * linear - quadratic * constant;
            if (discriminant >= 0.0f)
            {
                const auto distance = (-linear - std::sqrt(discriminant)) / quadratic;
                const auto along = offsetAlong + distance * directionAlong;
                if (distance >= 0.0f && along >= 0.0f && along <= squared)
                    keep(distance);
            }
        }
        return best && *best <= length ? best : std::nullopt;
    }

    std::optional<BodyHit> FindBodyHit(const glm::vec3& start, const glm::vec3& direction, float length,
                                       std::span<const Capsule> capsules)
    {
        std::optional<BodyHit> hit;
        for (const auto& capsule : capsules)
        {
            const auto distance =
                HitCapsule(start, direction, hit ? hit->distance : length, capsule.a, capsule.b, capsule.radius);
            if (!distance)
                continue;
            const auto point = start + direction * *distance;
            const auto outward = point - ClosestOnSegment(point, capsule.a, capsule.b);
            hit = BodyHit{*distance, point, glm::length(outward) > 0.0f ? glm::normalize(outward) : -direction,
                          capsule.part};
        }
        return hit;
    }

    bool PassesNear(const glm::vec3& start, const glm::vec3& direction, float length, const glm::vec3& point, float reach)
    {
        return glm::distance(point, ClosestOnSegment(point, start, start + direction * length)) <= reach;
    }

    Axes GetImpactAxes(const glm::vec3& normal, float turnDegrees)
    {
        // a line in the surface, as the game finds one (x, y, z swapped about), turned about the normal
        const glm::vec3 other(-normal.z, normal.x, -normal.y);
        auto left = other - normal * glm::dot(normal, other);
        if (glm::length(left) <= 0.0001f)
            left = std::abs(normal.z) < 0.9f ? glm::cross(normal, glm::vec3(0, 0, 1)) : glm::cross(normal, glm::vec3(1, 0, 0));
        left = glm::angleAxis(glm::radians(turnDegrees), normal) * glm::normalize(left);
        return {normal, left, glm::cross(normal, left)};
    }

    float GetImpactTurn(std::uint32_t tick)
    {
        constexpr std::uint32_t MIX = 2654435761u;
        return static_cast<float>((tick * MIX) >> 23) * 360.0f / 512.0f;
    }

    Spread GetSpread(std::int32_t shotCount, float aimedSpread, float chosen)
    {
        return {static_cast<std::uint32_t>(std::clamp<std::int32_t>(shotCount, 1, MOST_PELLETS)),
                chosen >= 0.0f ? chosen : aimedSpread > 0.0f ? aimedSpread : GUN_SCATTER};
    }

    glm::vec3 GetPelletDirection(const Axes& barrel, std::uint32_t tick, std::uint32_t pellet, float degrees)
    {
        // evenly over a disc: the root of one number for how far out, another for which way
        constexpr std::uint32_t MIX = 2654435761u, OTHER_MIX = 2246822519u;
        const auto seed = tick * 31u + pellet * 8191u + 1u;
        const auto out = static_cast<float>((seed * MIX) >> 16) / 65536.0f;
        const auto way = glm::radians(static_cast<float>((seed * OTHER_MIX) >> 16) / 65536.0f * 360.0f);
        const auto off = std::sqrt(out) * std::tan(glm::radians(degrees));
        return glm::normalize(barrel.forward + barrel.left * (off * std::cos(way)) + barrel.up * (off * std::sin(way)));
    }

    std::optional<glm::vec3> GetRocketPoint(const RocketFlight& flight, std::uint32_t tick)
    {
        if (tick < flight.tick || HasLanded(flight, tick))
            return std::nullopt;
        return flight.start + flight.direction * (flight.speed * static_cast<float>(tick - flight.tick) / 1000.0f);
    }

    bool HasLanded(const RocketFlight& flight, std::uint32_t tick)
    {
        return tick >= flight.tick &&
               flight.speed * static_cast<float>(tick - flight.tick) / 1000.0f >= flight.distance;
    }

    std::optional<glm::vec3> GetHomingPoint(const RocketFlight& flight, glm::vec3 target, std::uint32_t tick)
    {
        if (tick < flight.tick || HasHomed(flight, target, tick))
            return std::nullopt;
        const auto way = target - flight.start;
        const auto length = glm::length(way);
        const auto direction = length > 0.001f ? way / length : flight.direction;
        return flight.start + direction * (flight.speed * static_cast<float>(tick - flight.tick) / 1000.0f);
    }

    bool HasHomed(const RocketFlight& flight, glm::vec3 target, std::uint32_t tick)
    {
        return tick >= flight.tick &&
               flight.speed * static_cast<float>(tick - flight.tick) / 1000.0f >= glm::distance(flight.start, target);
    }

    bool IsFleshEntity(std::int32_t entityType)
    {
        constexpr std::array<std::int32_t, 4> FLESH = {1, 2, 17, 19};
        return std::ranges::find(FLESH, entityType) != FLESH.end();
    }
}  // namespace IWXMVM::Components::ShotLogic
