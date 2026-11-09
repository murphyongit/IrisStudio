#include "StdInclude.hpp"
#include "LiveGameLogic.hpp"

namespace IWXMVM::Components::LiveGameLogic
{
    const Killstreak* FindKillstreak(std::string_view type)
    {
        const auto found = std::ranges::find(KILLSTREAKS, type, &Killstreak::type);
        return found != KILLSTREAKS.end() ? &*found : nullptr;
    }

    bool FiresBullets(std::int32_t weaponType)
    {
        return weaponType == WEAPON_TYPE_BULLET;
    }

    std::int32_t GetBulletKind(bool rifleBullet)
    {
        return rifleBullet ? MOD_RIFLE_BULLET : MOD_PISTOL_BULLET;
    }

    std::int32_t GetDamage(const BulletDamage& bullet, float distance)
    {
        const auto span = bullet.farRange - bullet.nearRange;
        if (bullet.most == bullet.least || span <= 0.0f || distance <= bullet.nearRange)
            return bullet.most;
        if (distance >= bullet.farRange)
            return bullet.least;
        const auto along = (distance - bullet.nearRange) / span;
        return static_cast<std::int32_t>((1.0f - along) * static_cast<float>(bullet.most) +
                                         along * static_cast<float>(bullet.least));
    }

    bool IsEnemy(std::int32_t team, std::int32_t otherTeam, std::int32_t noTeam)
    {
        return team == noTeam || otherTeam == noTeam || team != otherTeam;
    }

    float GetDistanceToBody(const glm::vec3& point, const glm::vec3& origin)
    {
        const auto height = std::clamp(point.z - origin.z, 0.0f, BODY_HEIGHT);
        return glm::distance(point, origin + glm::vec3(0.0f, 0.0f, height));
    }

    std::vector<std::size_t> FindWithin(const glm::vec3& point, float radius, std::span<const glm::vec3> origins)
    {
        std::vector<std::pair<float, std::size_t>> found;
        for (std::size_t i = 0; i < origins.size(); ++i)
            if (const auto distance = GetDistanceToBody(point, origins[i]); distance <= radius)
                found.emplace_back(distance, i);
        std::ranges::sort(found);
        std::vector<std::size_t> nearest;
        nearest.reserve(found.size());
        for (const auto& [distance, index] : found)
            nearest.push_back(index);
        return nearest;
    }

    bool IsBodyDue(std::chrono::milliseconds age, std::optional<std::int32_t> timerSeconds)
    {
        return timerSeconds && age >= std::chrono::seconds(*timerSeconds);
    }

    std::optional<std::int32_t> GetBodyTimer(bool on, std::int32_t seconds)
    {
        if (!on)
            return std::nullopt;
        return std::clamp(seconds, BODY_SECONDS_LEAST, BODY_SECONDS_MOST);
    }
}  // namespace IWXMVM::Components::LiveGameLogic
