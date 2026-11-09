#include "StdInclude.hpp"
#include "LiveGame.hpp"

namespace IWXMVM::Components::LiveGame
{
    // The UI asks on the render thread, the game's main thread and its server's thread take it from here
    std::mutex mutex;
    Settings settings;
    std::vector<GunRequest> guns;
    std::vector<std::string> killstreaks;
    bool clearBodies = false;
    std::string problem;

    Settings GetSettings()
    {
        std::lock_guard lock(mutex);
        return settings;
    }

    void SetSettings(const Settings& chosen)
    {
        std::lock_guard lock(mutex);
        settings = chosen;
        settings.radius = std::clamp(settings.radius, LiveGameLogic::RADIUS_LEAST, LiveGameLogic::RADIUS_MOST);
        settings.bodySeconds =
            std::clamp(settings.bodySeconds, LiveGameLogic::BODY_SECONDS_LEAST, LiveGameLogic::BODY_SECONDS_MOST);
    }

    void RequestGun(GunRequest gun)
    {
        std::lock_guard lock(mutex);
        guns.push_back(std::move(gun));
    }

    std::vector<GunRequest> TakeGunRequests()
    {
        std::lock_guard lock(mutex);
        return std::exchange(guns, {});
    }

    void RequestKillstreak(std::string type)
    {
        std::lock_guard lock(mutex);
        killstreaks.push_back(std::move(type));
    }

    std::vector<std::string> TakeKillstreakRequests()
    {
        std::lock_guard lock(mutex);
        return std::exchange(killstreaks, {});
    }

    void RequestClearBodies()
    {
        std::lock_guard lock(mutex);
        clearBodies = true;
    }

    bool TakeClearBodies()
    {
        std::lock_guard lock(mutex);
        return std::exchange(clearBodies, false);
    }

    void SetProblem(std::string text)
    {
        std::lock_guard lock(mutex);
        problem = std::move(text);
    }

    std::string GetProblem()
    {
        std::lock_guard lock(mutex);
        return problem;
    }

    void DropRequests()
    {
        std::lock_guard lock(mutex);
        guns.clear();
        killstreaks.clear();
        clearBodies = false;
    }
}  // namespace IWXMVM::Components::LiveGame
