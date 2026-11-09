#include "StdInclude.hpp"
#include "DropLogic.hpp"

#include "ActorLogic.hpp"
#include "VehicleLogic.hpp"

namespace IWXMVM::Components::DropLogic
{
    using KillstreakLogic::FireDrop;

    namespace
    {
        constexpr float FALL_STEP = 0.05f;  // seconds between the points of a fall the ground is looked for along

        // Its model, the game's weapon napalm_mp, whose bomb keeps half the plane's speed; its script (_napalm.gsc,
        // callStrike_bombEffect) swaps the bomb for nothing half a second before it lands and plays the drop effect
        // where it was a tenth of a second later. It bursts on the ground (projExplosionEffect).
        constexpr std::string_view BOMB_MODEL = "projectile_napalm_bomb";
        constexpr float BOMB_SPEED = 0.5f;
        constexpr float BOMB_SWAP = 0.5f;
        constexpr std::uint32_t BOMB_DROP_EFFECT_AFTER = 100;
        constexpr std::string_view DROP_EFFECT = "weapon/napalm/fx_napalm_drop_mp";
        constexpr std::string_view BURST_EFFECT = "weapon/napalm/fx_napalm_ground_burst_mp";
        // The ground fires: seven, from where the bomb is 150 ms after that, 200 units apart along the way it goes,
        // each traced from there 60 degrees down; half a second apart, each less a tenth of a second late than the one
        // before; a small one at the ends and on steps, none on walls; they burn for 13 s
        constexpr std::uint32_t FIRES = 7;
        constexpr std::uint32_t FIRES_FROM = 150;
        constexpr float FIRE_APART = 200.0f;
        constexpr float FIRE_DOWN = 60.0f;
        constexpr float FIRE_TRACE = 10000.0f;
        constexpr float FIRE_BACK = 50.0f;
        constexpr float FIRE_STEP = 12.0f;
        constexpr float FIRE_STEEPEST = 0.5f;
        constexpr std::string_view SMALL_FIRE = "weapon/napalm/fx_napalm_ground_fire_sm_mp";
        constexpr std::string_view LARGE_FIRE = "weapon/napalm/fx_napalm_ground_fire_lg_mp";
        // (how long each goes on being seen for, for a playhead that lands inside it)
        constexpr std::uint32_t DROP_EFFECT_LIFE = 3000, BURST_LIFE = 6000, FIRE_LIFE = 13000;

        // The care package (_supplydrop.gsc, heliDropCrate): from the chopper's rear hatch, 400 behind it and 200 under
        // it. The chopper is stopped there in the game; here it may be going, and the crate goes on with it.
        constexpr float CRATE_BEHIND = 400.0f;
        constexpr float CRATE_UNDER = 200.0f;

        glm::vec3 GetForward(float yaw)
        {
            const auto radians = glm::radians(yaw);
            return {std::cos(radians), std::sin(radians), 0.0f};
        }

        // ms from a drop's swap to its ground fire: _napalm.gsc's traces half a second apart, then its wait in
        // napalmLosRadiusDamage, 0.5 s less a tenth for each that came before
        std::uint32_t GetFireDelay(std::uint32_t fire)
        {
            return FIRES_FROM + fire * 500 + (fire < 5 ? 500 - fire * 100 : 0);
        }

        void AddNapalm(Drop& drop, const Trace& trace)
        {
            const auto& release = drop.release;
            const auto seconds = drop.landing ? drop.landing->seconds : MOST_FALL;
            const auto swap = std::max(seconds - BOMB_SWAP, 0.1f);
            const auto swapTick = release.tick + static_cast<std::uint32_t>(std::lround(swap * 1000.0f));
            drop.goneAt = swapTick;
            if (!drop.landing)
                return;
            const auto yaw = release.yaw;
            drop.plays.push_back({DROP_EFFECT, swapTick + BOMB_DROP_EFFECT_AFTER, DROP_EFFECT_LIFE,
                                  GetPoint(release, swap), glm::vec3(0.0f, yaw, 0.0f)});
            const auto facing = ActorLogic::GetFacing(drop.landing->normal);
            const auto landTick = release.tick + static_cast<std::uint32_t>(std::lround(seconds * 1000.0f));
            drop.plays.push_back(
                {BURST_EFFECT, landTick, BURST_LIFE, drop.landing->point, glm::vec3(facing.x, facing.y, 0.0f)});

            // (where it is then, or where it was if it is down by then)
            const auto from = GetPoint(release, std::min(swap + FIRES_FROM / 1000.0f, seconds));
            const auto ahead = GetForward(yaw);
            const auto down = glm::cos(glm::radians(FIRE_DOWN)) * ahead -
                              glm::sin(glm::radians(FIRE_DOWN)) * glm::vec3(0.0f, 0.0f, 1.0f);
            auto previous = 0.0f;
            for (std::uint32_t fire = 0; fire < FIRES; ++fire)
            {
                const auto start = from - down * FIRE_BACK + ahead * (FIRE_APART * static_cast<float>(fire + 1));
                const auto hit = trace(start, start + down * FIRE_TRACE);
                if (!hit)
                    continue;
                const auto height = hit->point.z;
                if (hit->normal.z > FIRE_STEEPEST)
                {
                    const bool large = fire > 0 && fire + 1 < FIRES && std::abs(previous - height) < FIRE_STEP;
                    drop.plays.push_back({large ? LARGE_FIRE : SMALL_FIRE, swapTick + GetFireDelay(fire), FIRE_LIFE,
                                          hit->point, glm::vec3(270.0f, yaw - 180.0f, 0.0f)});
                }
                previous = height;
            }
        }
    }  // namespace

    glm::vec3 GetPoint(const Release& release, float seconds)
    {
        return release.point + release.velocity * seconds -
               glm::vec3(0.0f, 0.0f, 0.5f * VehicleLogic::GRAVITY * seconds * seconds);
    }

    std::optional<Landing> FindLanding(const Release& release, const Trace& trace)
    {
        auto from = release.point;
        for (auto seconds = FALL_STEP; seconds <= MOST_FALL; seconds += FALL_STEP)
        {
            const auto to = GetPoint(release, seconds);
            if (const auto hit = trace(from, to))
            {
                // (how far along this step it is, by its height: the step is short, the path nearly straight)
                const auto drop = from.z - to.z;
                const auto share = drop > 0.0f ? std::clamp((from.z - hit->point.z) / drop, 0.0f, 1.0f) : 1.0f;
                return Landing{seconds - FALL_STEP * (1.0f - share), hit->point, hit->normal};
            }
            from = to;
        }
        return std::nullopt;
    }

    std::vector<Drop> MakeDrops(const KillstreakLogic::Info& info, const Types::Actor& actor,
                                const Types::ActorEvent& event, const Place& place, const Trace& trace)
    {
        std::vector<Drop> drops;
        const bool bombs = info.fireDrop == FireDrop::NapalmBombs, crates = info.fireDrop == FireDrop::Crate;
        if (event.type != Types::ActorEventType::Fire || (!bombs && !crates) || !trace)
            return drops;
        constexpr std::uint32_t HALF_STEP = 25;
        const auto look = actor.models.empty() ? 0 : KillstreakLogic::GetLook(info, actor.models.front());
        for (std::uint32_t shot = 0; shot < std::min(event.shots, MOST_DROPS); ++shot)
        {
            const auto tick = event.tick + shot * event.interval;
            if (!ActorLogic::IsVisible(actor, tick))
                continue;
            // how it goes as it lets go
            const auto before = tick > HALF_STEP ? tick - HALF_STEP : 0u, after = tick + HALF_STEP;
            const auto at = place(tick);
            const auto going = (place(after).position - place(before).position) /
                               (static_cast<float>(after - before) / 1000.0f);
            Drop drop{.shot = shot, .release = {.tick = tick, .point = at.position, .yaw = at.yaw}};
            if (bombs)
            {
                drop.model = BOMB_MODEL;
                drop.release.velocity = going * BOMB_SPEED;
            }
            else
            {
                // (the friendly crate from the friendly chopper, the enemy's from the enemy's)
                const auto& crate = KillstreakLogic::Get(Types::Killstreak::CarePackage);
                drop.model = crate.models[std::min(look, crate.models.size() - 1)];
                drop.release.velocity = going;
                drop.release.point -= GetForward(at.yaw) * CRATE_BEHIND + glm::vec3(0.0f, 0.0f, CRATE_UNDER);
            }
            drop.landing = FindLanding(drop.release, trace);
            if (bombs)
                AddNapalm(drop, trace);
            drops.push_back(std::move(drop));
        }
        return drops;
    }

    std::uint32_t GetMortarFlight()
    {
        // (doMortarStrike: as fast as lands it MORTAR_DISTANCE away; callStrike_mortarBombEffect starts it partway)
        const auto pitch = glm::radians(MORTAR_PITCH);
        const auto speed = std::sqrt(VehicleLogic::GRAVITY * MORTAR_DISTANCE / std::sin(2.0f * pitch));
        const auto air = MORTAR_DISTANCE / (speed * std::cos(pitch));
        return static_cast<std::uint32_t>(std::lround(air * (1.0f - MORTAR_START_RATIO) * 1000.0f));
    }

    std::vector<Drop> MakeMortarDrops(const Types::Actor& actor, const Types::ActorEvent& event, const Trace& trace)
    {
        std::vector<Drop> drops;
        if (!actor.mortar || event.type != Types::ActorEventType::Fire || !trace || event.shots == 0)
            return drops;
        const auto pitch = glm::radians(MORTAR_PITCH);
        const auto speed = std::sqrt(VehicleLogic::GRAVITY * MORTAR_DISTANCE / std::sin(2.0f * pitch));
        const auto across = speed * std::cos(pitch), up = speed * std::sin(pitch);
        const auto air = MORTAR_DISTANCE / across;
        const auto ahead = GetForward(actor.yaw);
        const auto flight = GetMortarFlight();
        std::uint32_t shot = 0;
        for (std::size_t point = 0; point < actor.mortar->points.size(); ++point)
        {
            for (std::uint32_t shell = 0; shell < event.shots; ++shell, ++shot)
            {
                if (shot >= MOST_DROPS)
                    return drops;
                const auto lands =
                    event.tick + static_cast<std::uint32_t>(point) * MORTAR_POINT_APART + shell * event.interval;
                if (!ActorLogic::IsVisible(actor, lands))
                    continue;
                // where it is seen first along its arc, later on it for a shell landing before the film's start
                const auto late = lands < flight ? static_cast<float>(flight - lands) / 1000.0f : 0.0f;
                const auto seconds = air * MORTAR_START_RATIO + late;
                const auto site = actor.mortar->points[point];
                const auto fired = site - ahead * MORTAR_DISTANCE;
                Drop drop{.shot = shot, .model = MORTAR_MODEL};
                drop.release.tick = lands > flight ? lands - flight : 0u;
                drop.release.yaw = actor.yaw;
                drop.release.point =
                    fired + ahead * (across * seconds) +
                    glm::vec3(0.0f, 0.0f, up * seconds - 0.5f * VehicleLogic::GRAVITY * seconds * seconds);
                drop.release.velocity = ahead * across + glm::vec3(0.0f, 0.0f, up - VehicleLogic::GRAVITY * seconds);
                drop.landing = FindLanding(drop.release, trace);
                if (drop.landing)
                {
                    const auto landTick =
                        drop.release.tick + static_cast<std::uint32_t>(std::lround(drop.landing->seconds * 1000.0f));
                    drop.goneAt = landTick;
                    const auto facing = ActorLogic::GetFacing(drop.landing->normal);
                    drop.plays.push_back(
                        {MORTAR_BLAST, landTick, BURST_LIFE, drop.landing->point, glm::vec3(facing.x, facing.y, 0.0f)});
                }
                else
                    drop.goneAt = drop.release.tick + static_cast<std::uint32_t>(MOST_FALL * 1000.0f);
                drops.push_back(std::move(drop));
            }
        }
        return drops;
    }

    std::optional<Shown> GetShown(const Drop& drop, std::uint32_t tick)
    {
        const auto& release = drop.release;
        if (tick < release.tick || tick >= drop.goneAt)
            return std::nullopt;
        const auto seconds = static_cast<float>(tick - release.tick) / 1000.0f;
        if (!drop.landing && seconds > MOST_FALL)
            return std::nullopt;
        const auto ahead = GetForward(release.yaw);
        if (drop.landing && seconds >= drop.landing->seconds)
        {
            // at rest, lying on what it landed on
            const auto up = glm::normalize(drop.landing->normal);
            const auto forward = glm::normalize(ahead - up * glm::dot(ahead, up));
            return Shown{drop.landing->point, glm::mat3(forward, glm::cross(up, forward), up)};
        }
        Shown shown{GetPoint(release, seconds)};
        if (drop.goneAt == UINT32_MAX)
        {
            // (a crate falls level, turned as it left)
            const auto up = glm::vec3(0.0f, 0.0f, 1.0f);
            shown.rotation = glm::mat3(ahead, glm::cross(up, ahead), up);
            return shown;
        }
        // a bomb's nose goes the way it falls
        const auto going = glm::normalize(release.velocity - glm::vec3(0.0f, 0.0f, VehicleLogic::GRAVITY * seconds) +
                                          ahead * 0.001f);
        const auto side = glm::normalize(glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), going));
        shown.rotation = glm::mat3(going, side, glm::cross(going, side));
        return shown;
    }
}  // namespace IWXMVM::Components::DropLogic
