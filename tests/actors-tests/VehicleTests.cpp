#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ActorLogic.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/VehicleLogic.hpp"

using namespace IWXMVM;

// How a killstreak's body moves as it goes: helicopters pitch with speed and
// acceleration and bank into turns, hover drifting, planes bank and climb, a car's body leans, its wheels steer and
// roll; the corners of their paths are as wide as they turn

namespace
{
    namespace Killstreaks = Components::KillstreakLogic;
    namespace Vehicles = Components::VehicleLogic;
    using Types::Killstreak;

    const auto& COBRA = Killstreaks::Get(Killstreak::AttackHelicopter);
    const auto& RCXD = Killstreaks::Get(Killstreak::RcXd);
    const auto& NAPALM = Killstreaks::Get(Killstreak::Napalm);

    // along x from the origin at a speed, speeding up at a rate (units a second, a second squared), facing x
    Vehicles::PlacementAt Straight(float speed, float acceleration, float climb = 0.0f)
    {
        return [=](std::uint32_t tick) {
            const auto seconds = static_cast<float>(tick) / 1000.0f;
            const auto along = speed * seconds + 0.5f * acceleration * seconds * seconds;
            return Types::ActorPlacement{glm::vec3(along, 0.0f, climb * seconds), 0.0f};
        };
    }

    // round a circle of a radius about the origin at a speed, to the left (anticlockwise), facing the way it goes
    Vehicles::PlacementAt Circle(float radius, float speed)
    {
        return [=](std::uint32_t tick) {
            const auto angle = speed * static_cast<float>(tick) / 1000.0f / radius;
            return Types::ActorPlacement{glm::vec3(radius * std::sin(angle), radius - radius * std::cos(angle), 0.0f),
                                         glm::degrees(angle)};
        };
    }

    // still where it is
    Vehicles::PlacementAt Still()
    {
        return [](std::uint32_t) { return Types::ActorPlacement{glm::vec3(100.0f, 200.0f, 1000.0f), 30.0f}; };
    }

    // the motions as Components::Actors looks them up
    ActorLogic::AnimationLookup Animations()
    {
        return [](const std::string& name) -> std::optional<ActorLogic::AnimationInfo> {
            if (Killstreaks::IsMotion(name))
                return Killstreaks::GetMotionInfo(name);
            return std::nullopt;
        };
    }
}  // namespace

TEST("vehicles: a helicopter's nose goes down with its speed and as it speeds up, up as it slows down")
{
    // half its most pitch at its fastest preset (60 mph), a quarter at 30
    const auto cruising = Vehicles::GetBody(COBRA, 10000, Straight(Killstreaks::FromMph(30.0f), 0.0f), 0.0f, 1);
    CHECK_NEAR(cruising.pitch, COBRA.maxPitch * 0.25f, 0.05f);
    CHECK_NEAR(cruising.roll, 0.0f, 0.01f);
    CHECK(glm::length(cruising.drift) == 0.0f);

    // speeding up at its acceleration adds the tilt that pushes it on (atan(a / g))
    const auto speeding = Vehicles::GetBody(COBRA, 1000, Straight(0.0f, COBRA.acceleration), 0.0f, 1);
    const auto speed = COBRA.acceleration * 1.0f;
    CHECK_NEAR(speeding.pitch,
               COBRA.maxPitch * 0.5f * speed / Killstreaks::FromMph(60.0f) +
                   glm::degrees(std::atan2(COBRA.acceleration, Vehicles::GRAVITY)),
               0.05f);
    // slowing down raises its nose
    const auto slowing = Vehicles::GetBody(COBRA, 1000, Straight(Killstreaks::FromMph(10.0f), -COBRA.acceleration), 0.0f, 1);
    CHECK(slowing.pitch < 0.0f);
    // never more than its most
    const auto racing = Vehicles::GetBody(COBRA, 1000, Straight(Killstreaks::FromMph(60.0f), 5000.0f), 0.0f, 1);
    CHECK_NEAR(racing.pitch, COBRA.maxPitch, 1e-4);
}

TEST("vehicles: a helicopter banks into turns, a plane too, and climbs nose up")
{
    // turning left at the radius it turns at banks it to the left (its left side down: negative) about as far as it
    // banks, a little less for the time its tilt follows over
    const auto speed = Killstreaks::FromMph(40.0f);
    const auto radius = Vehicles::GetTurnRadius(COBRA, speed);
    CHECK_NEAR(radius, speed * speed / (Vehicles::GRAVITY * std::tan(glm::radians(30.0f))), 0.5f);
    const auto turning = Vehicles::GetBody(COBRA, 20000, Circle(radius, speed), 0.0f, 1);
    CHECK(turning.roll < -COBRA.maxRoll * 0.9f && turning.roll >= -COBRA.maxRoll);
    const auto right = Vehicles::GetBody(COBRA, 20000, Circle(-radius, speed), 0.0f, 1);
    CHECK_NEAR(right.roll, -turning.roll, 0.01f);

    // a plane banks the same way, and climbing raises its nose
    const auto banking = Vehicles::GetBody(NAPALM, 20000, Circle(60000.0f, 7000.0f), 0.0f, 1);
    CHECK_NEAR(banking.roll, -glm::degrees(std::atan2(7000.0f * 7000.0f / 60000.0f, Vehicles::GRAVITY)), 1.0f);
    const auto climbing = Vehicles::GetBody(NAPALM, 20000, Straight(7000.0f, 0.0f, 700.0f), 0.0f, 1);
    CHECK_NEAR(climbing.pitch, -glm::degrees(std::atan2(700.0f, 7000.0f)), 0.01f);
    CHECK_NEAR(climbing.roll, 0.0f, 0.01f);
}

TEST("vehicles: a hovering helicopter drifts about where it is, slowly, out of step with another")
{
    // within its hover drift, never quickly, the same at the same tick
    float most = 0.0f, fastest = 0.0f;
    glm::vec3 last = Vehicles::GetBody(COBRA, 5000, Still(), 0.0f, 7).drift;
    for (std::uint32_t tick = 5010; tick < 65000; tick += 10)
    {
        const auto drift = Vehicles::GetBody(COBRA, tick, Still(), 0.0f, 7).drift;
        most = std::max(most, glm::length(glm::vec2(drift)));
        fastest = std::max(fastest, glm::distance(drift, last) * 100.0f);
        last = drift;
    }
    CHECK(most > COBRA.hoverDrift * 0.5f && most <= COBRA.hoverDrift);
    CHECK(fastest < 100.0f);  // SetHoverParams' speed
    CHECK(Vehicles::GetBody(COBRA, 30000, Still(), 0.0f, 7).drift ==
          Vehicles::GetBody(COBRA, 30000, Still(), 0.0f, 7).drift);
    CHECK(glm::distance(Vehicles::GetBody(COBRA, 30000, Still(), 0.0f, 7).drift,
                        Vehicles::GetBody(COBRA, 30000, Still(), 0.0f, 8).drift) > 1.0f);
    // it rocks with its drift to its side and bobs, as the game's Attack Helicopter measured
    // (up to 11 degrees of roll, 37 units up and down), smoothly; its nose stays about level
    float rocked = 0.0f, bobbed = 0.0f, pitched = 0.0f, step = 0.0f;
    auto lastRoll = Vehicles::GetBody(COBRA, 5000, Still(), 0.0f, 7).roll;
    for (std::uint32_t tick = 5016; tick < 65000; tick += 16)
    {
        const auto body = Vehicles::GetBody(COBRA, tick, Still(), 0.0f, 7);
        rocked = std::max(rocked, std::abs(body.roll));
        bobbed = std::max(bobbed, std::abs(body.drift.z));
        pitched = std::max(pitched, std::abs(body.pitch));
        step = std::max(step, std::abs(body.roll - lastRoll));
        lastRoll = body.roll;
    }
    CHECK(rocked > 8.0f && rocked < 13.0f);
    CHECK(bobbed > 30.0f && bobbed < 40.0f);
    CHECK(pitched < 5.0f);
    CHECK(step < 0.3f);
    // going on, it doesn't drift
    CHECK(glm::length(Vehicles::GetBody(COBRA, 30000, Straight(Killstreaks::FromMph(40.0f), 0.0f), 0.0f, 7).drift) == 0.0f);
}

TEST("vehicles: a car's body leans out of turns and squats, its front wheels steer and its wheels roll")
{
    // turning left: steered left as much as turns it round the circle, leaning right (right side down: positive)
    const auto speed = Killstreaks::FromMph(9.5f);
    const auto turning = Vehicles::GetBody(RCXD, 20000, Circle(100.0f, speed), 250.0f, 1);
    const auto wheelbase = RCXD.wheels.front + RCXD.wheels.back;
    CHECK_NEAR(turning.steer, glm::degrees(std::atan(wheelbase / 100.0f)), 0.2f);
    CHECK_NEAR(turning.bodyRoll,
               RCXD.maxRoll * speed * speed / 100.0f / (RCXD.wheels.grip * Vehicles::GRAVITY), 0.2f);
    CHECK(turning.pitch == 0.0f && turning.roll == 0.0f);  // the ground's, worked out in the game
    // the wheels have turned as far as they rolled
    CHECK_NEAR(turning.wheelTurn, std::fmod(glm::degrees(250.0f / RCXD.wheels.radius), 360.0f), 0.01f);
    // steering as tight as it goes
    CHECK_NEAR(Vehicles::GetBody(RCXD, 20000, Circle(5.0f, 20.0f), 0.0f, 1).steer, RCXD.wheels.maxSteer, 1e-4);

    // speeding up squats it (nose up: negative), slowing down dips it
    CHECK(Vehicles::GetBody(RCXD, 300, Straight(0.0f, RCXD.acceleration), 0.0f, 1).bodyPitch < -RCXD.maxPitch * 0.4f);
    CHECK(Vehicles::GetBody(RCXD, 300, Straight(300.0f, -RCXD.acceleration), 0.0f, 1).bodyPitch > RCXD.maxPitch * 0.4f);
    const auto straight = Vehicles::GetBody(RCXD, 300, Straight(300.0f, 0.0f), 0.0f, 1);
    CHECK(std::abs(straight.bodyPitch) < 1e-3 && std::abs(straight.steer) < 1e-3);

    // the bones that does that: its body, its front wheels' steering, the four wheels rolling
    const auto joints = Vehicles::GetJoints(RCXD, turning);
    CHECK(joints.size() == 7 && joints[0].bone == "tag_body" && joints[1].bone == "tag_wheel_front_left" &&
          joints[3].bone == "left_wheel_01_jnt");
    CHECK(joints[0].angles == glm::vec3(turning.bodyPitch, 0.0f, turning.bodyRoll));
    CHECK(joints[1].angles == glm::vec3(0.0f, turning.steer, 0.0f));
    CHECK(joints[6].angles == glm::vec3(turning.wheelTurn, 0.0f, 0.0f));
    CHECK(Vehicles::GetJoints(COBRA, turning).empty());

    // it turns as tight as it steers, wider than its tyres hold at speed
    CHECK_NEAR(Vehicles::GetTurnRadius(RCXD, 10.0f), wheelbase / std::tan(glm::radians(40.0f)), 0.01f);
    CHECK_NEAR(Vehicles::GetTurnRadius(RCXD, Killstreaks::FromMph(19.0f)),
               Killstreaks::FromMph(19.0f) * Killstreaks::FromMph(19.0f) / (1.1f * Vehicles::GRAVITY), 0.01f);
}

TEST("vehicles: a helicopter's path turns as wide as it banks, and it banks round its corner")
{
    auto actor = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 0, 60000, 0.0f);
    actor.path = {glm::vec3(0.0f, 0.0f, 1100.0f), glm::vec3(4000.0f, 0.0f, 1100.0f),
                  glm::vec3(4000.0f, 4000.0f, 1100.0f)};
    const auto animations = Animations();
    ActorLogic::FitWalk(actor, actor.clips.front().id, animations, actor.pathSpeed);
    const auto radius = Vehicles::GetTurnRadius(COBRA, actor.pathSpeed);
    CHECK(radius > 1000.0f);
    const auto rounded = ActorLogic::GetRoundedPath(actor, animations);
    const auto arc = std::ranges::find_if(rounded.pieces, [](const auto& piece) { return piece.radius > 0.0f; });
    CHECK(arc != rounded.pieces.end() && std::abs(arc->radius - radius) < 1.0f);
    // people's corners are as they were
    CHECK_NEAR(ActorLogic::GetCornerRadius(Types::Actor{}, 211.0f), ActorLogic::GetCornerRadius(211.0f), 1e-4);

    // halfway round it, banked to the left nearly as far as it banks
    if (arc == rounded.pieces.end())
        return;
    const auto middle = arc->distance + arc->length / 2.0f;
    std::uint32_t tick = 0;
    while (tick < 60000 && ActorLogic::GetPathProgress(actor, tick, animations) < middle)
        tick += 10;
    const auto body = Vehicles::GetBody(
        COBRA, tick, [&](std::uint32_t at) { return ActorLogic::EvaluatePlacement(actor, at, animations); },
        ActorLogic::GetPathProgress(actor, tick, animations), actor.id);
    CHECK(body.roll < -COBRA.maxRoll * 0.85f && body.roll >= -COBRA.maxRoll);
}

TEST("vehicles: an RC-XD steers as its path's corner turns it, halfway round it")
{
    // at half speed, turning right
    auto actor = Killstreaks::MakeStart(Killstreak::RcXd, 0, 60000, 0.0f);
    actor.pathSpeed = Killstreaks::FromMph(9.5f);
    actor.path = {glm::vec3(0.0f), glm::vec3(200.0f, 0.0f, 0.0f), glm::vec3(200.0f, -250.0f, 0.0f)};
    const auto animations = Animations();
    ActorLogic::FitWalk(actor, actor.clips.front().id, animations, actor.pathSpeed);
    const auto rounded = ActorLogic::GetRoundedPath(actor, animations);
    const auto arc = std::ranges::find_if(rounded.pieces, [](const auto& piece) { return piece.radius > 0.0f; });
    CHECK(arc != rounded.pieces.end() && std::abs(arc->radius - Vehicles::GetTurnRadius(RCXD, actor.pathSpeed)) < 0.1f);
    if (arc == rounded.pieces.end())
        return;
    const auto middle = arc->distance + arc->length / 2.0f;
    std::uint32_t tick = 0;
    while (tick < 60000 && ActorLogic::GetPathProgress(actor, tick, animations) < middle)
        ++tick;
    const auto body = Vehicles::GetBody(
        RCXD, tick, [&](std::uint32_t at) { return ActorLogic::EvaluatePlacement(actor, at, animations); },
        ActorLogic::GetPathProgress(actor, tick, animations), actor.id);
    CHECK_NEAR(body.steer, -glm::degrees(std::atan((RCXD.wheels.front + RCXD.wheels.back) / arc->radius)), 1.0f);
    CHECK(body.bodyRoll < -1.0f);
}

TEST("vehicles: guns aim within their arcs, scan between them and spin up as they fire")
{
    const auto& sentry = Killstreaks::Get(Killstreak::SentryGun);
    const auto& gun = sentry.guns.front();
    CHECK(sentry.guns.size() == 1 && gun.yaw == "tag_aim" && gun.pitch == "tag_aim_pitch" && sentry.spin == "tag_spin");
    // toward a point ahead, left and above: yaw left, pitch up (negative)
    const auto aim = Vehicles::AimAt(gun, glm::vec3(100.0f, 100.0f, 50.0f));
    CHECK_NEAR(aim.yaw, 45.0f, 1e-3);
    CHECK_NEAR(aim.pitch, -glm::degrees(std::atan2(50.0f, std::sqrt(2.0f) * 100.0f)), 1e-3);
    // no further than its arcs: behind it stops at its right arc (55), far below at 20 down
    CHECK_NEAR(Vehicles::AimAt(gun, glm::vec3(-100.0f, -10.0f, 0.0f)).yaw, -55.0f, 1e-3);
    CHECK_NEAR(Vehicles::AimAt(gun, glm::vec3(10.0f, 0.0f, -100.0f)).pitch, 20.0f, 1e-3);
    // the SAM turns all the way round; the Huey's door gun 110 either way
    const auto& sam = Killstreaks::Get(Killstreak::SamTurret).guns.front();
    CHECK_NEAR(Vehicles::AimAt(sam, glm::vec3(-100.0f, -10.0f, 0.0f)).yaw, glm::degrees(std::atan2(-10.0f, -100.0f)),
               1e-3);
    const auto& door = Killstreaks::Get(Killstreak::ChopperGunner).guns.front();
    CHECK_NEAR(Vehicles::AimAt(door, glm::vec3(0.0f, 100.0f, 0.0f)).yaw, 90.0f, 1e-3);
    CHECK_NEAR(Vehicles::AimAt(door, glm::vec3(-100.0f, 10.0f, 0.0f)).yaw, 110.0f, 1e-3);

    // scanning: out to 55 left in 55 / 60 s, across to 55 right, back to the middle, level
    CHECK_NEAR(Vehicles::GetScan(gun, 0.5f).yaw, 30.0f, 1e-3);
    CHECK_NEAR(Vehicles::GetScan(gun, 55.0f / 60.0f).yaw, 55.0f, 1e-3);
    CHECK_NEAR(Vehicles::GetScan(gun, (55.0f + 110.0f) / 60.0f).yaw, -55.0f, 1e-3);
    CHECK_NEAR(Vehicles::GetScan(gun, 220.0f / 60.0f).yaw, 0.0f, 1e-3);
    CHECK(Vehicles::GetScan(gun, 1.3f).pitch == 0.0f);
    CHECK_NEAR(Vehicles::GetScan(sam, 9.0f).yaw, 90.0f, 1e-3);
    // a Sentry Gun's start scans; its Look events blend from there
    const auto turret = Killstreaks::MakeStart(Killstreak::SentryGun, 1000, 30000, 0.0f);
    const auto scanning = Vehicles::GetIdleAim(gun, turret, 1500);
    CHECK(scanning && std::abs(scanning->yaw - 30.0f) < 1e-3);
    // before its Scan clip, and a gun without one, are left to their animations
    CHECK(!Vehicles::GetIdleAim(gun, turret, 500));
    const auto huey = Killstreaks::MakeStart(Killstreak::ChopperGunner, 1000, 30000, 0.0f);
    CHECK(!Vehicles::GetIdleAim(Killstreaks::Get(Killstreak::ChopperGunner).guns.front(), huey, 1500));
    const auto half = Vehicles::Blend({170.0f, 0.0f}, {-170.0f, 10.0f}, 0.5f);
    CHECK(std::abs(std::remainder(half.yaw - 180.0f, 360.0f)) < 1e-3 && std::abs(half.pitch - 5.0f) < 1e-4);

    // the barrels spin up over a second as it fires and down after its last shot
    const std::vector<Types::ActorEvent> events{{.type = Types::ActorEventType::Fire, .tick = 1000, .shots = 40,
                                                 .interval = 50}};
    CHECK(Vehicles::GetSpin(events, 1000, 50) == 0.0f);
    CHECK_NEAR(Vehicles::GetSpin(events, 1500, 50), Vehicles::SPIN_RATE * 0.125f, 0.01f);
    const auto spun = [&](std::uint32_t tick) { return Vehicles::GetSpin(events, tick, 50); };
    // (at full speed after a second: 1080 a second, 27 a 25 ms frame)
    CHECK_NEAR(std::remainder(spun(2525) - spun(2500), 360.0f), 27.0f, 0.01f);
    CHECK(std::remainder(spun(5025) - spun(5000), 360.0f) == 0.0f);
}

TEST("vehicles: the care package falls from the chopper to the ground")
{
    const auto crate = Killstreaks::MakeStart(Killstreak::CarePackage, 1000, 30000, 0.0f);
    CHECK_NEAR(Vehicles::GetDropHeight(crate, 1000), Killstreaks::DROP_HEIGHT, 1e-3);
    CHECK_NEAR(Vehicles::GetDropHeight(crate, 1500), Killstreaks::DROP_HEIGHT - 0.5f * Vehicles::GRAVITY * 0.25f, 1e-3);
    CHECK(Vehicles::GetDropHeight(crate, 1000 + Vehicles::GetDropTicks() - 1) < 2.0f);
    CHECK(Vehicles::GetDropHeight(crate, 5000) == 0.0f);
    CHECK(Vehicles::GetDropTicks() == 1275);
}

// Flights: from rest to rest at the killstreak's acceleration, soft corners, a
// lower peak on a short hop, Fly here carrying the speed on; the tilt from a bell-shaped window

namespace
{
    // where an actor is at a tick, as the game draws it
    Vehicles::PlacementAt PlacementOf(const Types::Actor& actor)
    {
        return [&actor](std::uint32_t tick) { return ActorLogic::EvaluatePlacement(actor, tick, Animations()); };
    }

    // how fast it goes over the 10 ms from a tick, along its path (units a second)
    float SpeedOf(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto at = PlacementOf(actor);
        return glm::distance(at(tick + 10).position, at(tick).position) * 100.0f;
    }

    // the most its speed changes between 10 ms samples and its pitch between 16 ms frames from one tick to another, and
    // the fastest it goes
    struct Steps
    {
        float speed = 0.0f;
        float pitch = 0.0f;
        float fastest = 0.0f;
    };
    Steps GetSteps(const Types::Actor& actor, const Killstreaks::Info& info, std::uint32_t from, std::uint32_t to)
    {
        Steps steps;
        const auto animations = Animations();
        auto speed = SpeedOf(actor, from);
        for (auto tick = from + 10; tick < to; tick += 10)
        {
            const auto now = SpeedOf(actor, tick);
            steps.speed = std::max(steps.speed, std::abs(now - speed));
            steps.fastest = std::max(steps.fastest, now);
            speed = now;
        }
        const auto pitchAt = [&](std::uint32_t tick) {
            return Vehicles::GetBody(info, tick, PlacementOf(actor),
                                     ActorLogic::GetPathProgress(actor, tick, animations), actor.id)
                .pitch;
        };
        auto pitch = pitchAt(from);
        for (auto tick = from + 16; tick < to; tick += 16)
        {
            const auto now = pitchAt(tick);
            steps.pitch = std::max(steps.pitch, std::abs(now - pitch));
            pitch = now;
        }
        return steps;
    }
}  // namespace

TEST("vehicles: a change of speed ramps along a soft S at the acceleration")
{
    // the Cobra to its cruise: 3.2 s at 12.5 mph a second, 0.3 s more for its soft corners; a small change as fast as
    // it gets
    CHECK_NEAR(Vehicles::GetRampTime(0.0f, 704.0f, COBRA.acceleration), 3.5f, 1e-3);
    CHECK_NEAR(Vehicles::GetRampTime(704.0f, 0.0f, COBRA.acceleration), 3.5f, 1e-3);
    CHECK_NEAR(Vehicles::GetRampTime(100.0f, 122.0f, COBRA.acceleration), 0.2f, 1e-3);
    CHECK(Vehicles::GetRampTime(0.0f, 704.0f, 0.0f) == 0.0f);
    CHECK(Killstreaks::GetSpeedChangeTicks(COBRA, 704.0f) == 3500);
    // the share made: none, half at its middle, all; its acceleration never more than the killstreak's
    const auto time = 3.5f;
    CHECK(Vehicles::GetRampShare(0.0f, time) == 0.0f && Vehicles::GetRampShare(time, time) == 1.0f);
    CHECK_NEAR(Vehicles::GetRampShare(time / 2.0f, time), 0.5f, 1e-5);
    float most = 0.0f, last = 0.0f;
    for (float t = 0.01f; t <= time + 0.01f; t += 0.01f)
    {
        const auto share = Vehicles::GetRampShare(t, time);
        most = std::max(most, (share - last) * 704.0f / 0.01f);
        CHECK(share >= last);
        last = share;
    }
    CHECK(most <= COBRA.acceleration * 1.01f && most > COBRA.acceleration * 0.95f);
    // as far as half its time at the speed it ends at, and its area matches its share
    CHECK_NEAR(Vehicles::GetRampArea(time, time), time / 2.0f, 1e-5);
    CHECK_NEAR(Vehicles::GetRampArea(time + 1.0f, time), time / 2.0f + 1.0f, 1e-5);
    float sum = 0.0f;
    for (float t = 0.0005f; t < 1.0f; t += 0.001f)
        sum += Vehicles::GetRampShare(t, time) * 0.001f;
    CHECK_NEAR(sum, Vehicles::GetRampArea(1.0f, time), 1e-4);
}

TEST("vehicles: a long flight cruises at its speed, a short one peaks lower, and both arrive at rest")
{
    const auto animations = Animations();
    // 4000 units: speeding up over the Fly's blend, at its speed, slowing over the Hover's, at rest at the end
    auto actor = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 1000, 60000, 0.0f);
    actor.path = {glm::vec3(0.0f, 0.0f, 1100.0f), glm::vec3(4000.0f, 0.0f, 1100.0f)};
    ActorLogic::FitWalk(actor, actor.clips.front().id, animations, actor.pathSpeed);
    const auto& fly = actor.clips[0];
    const auto& hover = actor.clips[1];
    CHECK(fly.blendIn == 3500 && hover.blendIn == 3500 && hover.start == fly.End());
    CHECK_NEAR(SpeedOf(actor, fly.start + fly.blendIn + 100), 704.0f, 1.0f);
    const auto halfway = SpeedOf(actor, fly.start + fly.blendIn / 2);
    CHECK(halfway > 704.0f * 0.45f && halfway < 704.0f * 0.55f);
    const auto rest = hover.start + hover.blendIn;
    CHECK_NEAR(ActorLogic::EvaluatePlacement(actor, rest, animations).position.x, 4000.0f, 0.5f);
    CHECK(ActorLogic::EvaluatePlacement(actor, rest - 500, animations).position.x < 4000.0f - 2.0f);
    CHECK(SpeedOf(actor, rest) < 0.5f && SpeedOf(actor, fly.start - 20) == 0.0f);
    // (its arrival as the path bar has it)
    const auto timing = ActorLogic::GetPathTiming(actor, animations);
    CHECK(timing.arrival && *timing.arrival > rest - 300 && *timing.arrival <= rest);

    // a short hop: 788 units climbing 150, as its starting point and the path placed make it; it peaks at
    // about 385 u/s and takes about 4.1 s, at rest at the end
    auto hop = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 31250, 90000, 0.0f);
    hop.path = {glm::vec3(0.0f, 0.0f, 1100.0f), glm::vec3(774.0f, 0.0f, 1250.0f)};
    ActorLogic::FitWalk(hop, hop.clips.front().id, animations, hop.pathSpeed);
    const auto end = hop.clips[1].start + hop.clips[1].blendIn;
    CHECK(end - hop.clips[0].start > 4000 && end - hop.clips[0].start < 4200);
    const auto steps = GetSteps(hop, COBRA, 31000, end + 500);
    CHECK(steps.fastest > 370.0f && steps.fastest < 400.0f);
    CHECK(glm::distance(ActorLogic::EvaluatePlacement(hop, end, animations).position,
                        glm::vec3(774.0f, 0.0f, 1250.0f)) < 0.5f);
    CHECK(SpeedOf(hop, end) < 0.5f);

    // the RC-XD drives the same way at its own rate
    auto car = Killstreaks::MakeStart(Killstreak::RcXd, 1000, 30000, 0.0f);
    car.path = {glm::vec3(0.0f), glm::vec3(1000.0f, 0.0f, 0.0f)};
    ActorLogic::FitWalk(car, car.clips.front().id, animations, car.pathSpeed);
    CHECK(car.clips[0].blendIn == Killstreaks::GetSpeedChangeTicks(RCXD, car.pathSpeed));
    CHECK_NEAR(ActorLogic::EvaluatePlacement(car, car.clips[1].start + car.clips[1].blendIn, animations).position.x,
               1000.0f, 0.5f);
    CHECK(GetSteps(car, RCXD, 900, car.clips[1].start + car.clips[1].blendIn + 200).speed < 10.0f);
}

TEST("vehicles: the owner's two flights have no speed step over 10 u/s in 10 ms nor a pitch step over 1 degree")
{
    const auto animations = Animations();
    // the first: a start, its second point placed 788 units on and 150 up (it set off at full speed and stopped dead)
    auto hop = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 31250, 90000, 0.0f);
    hop.path = {glm::vec3(0.0f, 0.0f, 1100.0f), glm::vec3(774.0f, 0.0f, 1250.0f)};
    ActorLogic::FitWalk(hop, hop.clips.front().id, animations, hop.pathSpeed);
    const auto first = GetSteps(hop, COBRA, 30000, 37000);
    CHECK(first.speed < 10.0f);
    CHECK(first.pitch < 1.0f);

    // the second: Fly here twice, the first while it still slows to its hover, the second while it speeds up again
    auto actor = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 10000, 90000, 0.0f);
    actor.path = {glm::vec3(0.0f, 0.0f, 1100.0f), glm::vec3(-1300.0f, 0.0f, 1380.0f)};
    ActorLogic::FitWalk(actor, actor.clips.front().id, animations, actor.pathSpeed);
    const auto slowing = actor.clips[1].start + 1000;
    const auto fly = std::string(Killstreaks::MOTION_FLY), hover = std::string(Killstreaks::MOTION_HOVER);
    const auto before = SpeedOf(actor, slowing - 10);
    const auto going = ActorLogic::GetPathProgress(actor, slowing, animations);
    CHECK(before > 50.0f);
    const auto second = ActorLogic::WalkTo(actor, slowing, glm::vec3(-1300.0f, 1500.0f, 1300.0f), fly, hover,
                                           animations, actor.pathSpeed);
    CHECK(second != 0);
    // (the way there as it was, and on at the speed it had)
    CHECK_NEAR(ActorLogic::GetPathProgress(actor, slowing, animations), going, 0.5f);
    CHECK(std::abs(SpeedOf(actor, slowing - 10) - before) < 1.0f);
    CHECK(std::abs(SpeedOf(actor, slowing) - before) < 10.0f);
    const auto speedingUp = slowing + 1500;
    CHECK(SpeedOf(actor, speedingUp) > before);
    ActorLogic::WalkTo(actor, speedingUp, glm::vec3(0.0f, 1500.0f, 1200.0f), fly, hover, animations, actor.pathSpeed);
    const auto& rest = actor.clips.back();
    const auto end = rest.start + rest.blendIn;
    const auto twice = GetSteps(actor, COBRA, 9000, end + 1000);
    CHECK(twice.speed < 10.0f);
    CHECK(twice.pitch < 1.0f);
    // at rest where it was flown to last
    CHECK(glm::distance(ActorLogic::EvaluatePlacement(actor, end, animations).position,
                        glm::vec3(0.0f, 1500.0f, 1200.0f)) < 0.5f);
    CHECK(SpeedOf(actor, end) < 0.5f);
}

TEST("vehicles: a step in speed swings a helicopter's tilt smoothly, never at once")
{
    // 300 u/s at once at 10 s: the bell-shaped window turns it into a swing over its tilt time
    const auto stepped = [](std::uint32_t tick) {
        const auto seconds = static_cast<float>(tick) / 1000.0f;
        return Types::ActorPlacement{glm::vec3(seconds > 10.0f ? (seconds - 10.0f) * 300.0f : 0.0f, 0.0f, 1000.0f),
                                     0.0f};
    };
    float most = 0.0f, highest = 0.0f;
    auto last = Vehicles::GetBody(COBRA, 8000, stepped, 0.0f, 1).pitch;
    for (std::uint32_t tick = 8016; tick < 12000; tick += 16)
    {
        const auto pitch = Vehicles::GetBody(COBRA, tick, stepped, 0.0f, 1).pitch;
        most = std::max(most, std::abs(pitch - last));
        highest = std::max(highest, pitch);
        last = pitch;
    }
    CHECK(most < 1.0f);
    // (it still tilts as it speeds up: most at the step)
    CHECK(highest > 10.0f);
}

TEST("vehicles: dust under a helicopter on the timeline's ticks by its height, and the RC-XD's treads by its speed")
{
    // every 50 ms near the ground, every 100 at 800 up, every 150 at its highest, none higher; only on multiples of 50
    const auto count = [](float height) {
        int dust = 0;
        for (std::uint32_t tick = 0; tick < 3000; ++tick)
            dust += Vehicles::IsDustTick(tick, height) ? 1 : 0;
        return dust;
    };
    CHECK(count(100.0f) == 60 && count(Vehicles::DUST_LOWEST) == 60);
    CHECK(count(775.0f) == 30);
    CHECK(count(Vehicles::DUST_HIGHEST) == 20);
    CHECK(count(Vehicles::DUST_HIGHEST + 1.0f) == 0);
    CHECK(!Vehicles::IsDustTick(25, 100.0f));

    // the RC-XD: driving at a quarter of its top speed or more, slower but moving, a trail at three quarters
    const auto treads = [](float speed, float ahead = 0.0f, float aside = 0.0f) {
        Vehicles::Body body;
        body.speed = speed;
        body.ahead = ahead;
        body.aside = aside;
        return Vehicles::GetTreads(RCXD, body);
    };
    using Tread = Vehicles::Tread;
    CHECK(treads(0.0f).empty());
    CHECK((treads(50.0f) == std::vector{Tread::Slow}));
    CHECK((treads(150.0f) == std::vector{Tread::Driving}));
    CHECK((treads(300.0f) == std::vector{Tread::Driving, Tread::Trail}));
    CHECK((treads(-50.0f) == std::vector{Tread::Reverse}));
    // peeling out (as hard as it speeds up, slowly yet) and sliding (as hard as its tyres hold) leave a trail too
    CHECK((treads(60.0f, RCXD.acceleration) == std::vector{Tread::Peel, Tread::Slow, Tread::Trail}));
    CHECK((treads(200.0f, 0.0f, RCXD.wheels.grip * Vehicles::GRAVITY) ==
           std::vector{Tread::Drift, Tread::Driving, Tread::Trail}));
    CHECK((Vehicles::GetTreads(COBRA, Vehicles::Body{.speed = 300.0f}).empty()));
    CHECK(Vehicles::GetTreadEffect("dust", Tread::Driving) == "vehicle/treadfx/fx_treadfx_rcbomb_dust");
    CHECK(Vehicles::GetTreadEffect("snow", Tread::Peel) == "vehicle/treadfx/fx_treadfx_rcbomb_snow_peel");

    // the speed and accelerations its body is measured with: a car driving on at 200 u/s
    const auto driving = Vehicles::GetBody(RCXD, 2000, Straight(200.0f, 0.0f), 0.0f, 1);
    CHECK_NEAR(driving.speed, 200.0f, 0.5f);
    CHECK(std::abs(driving.ahead) < 1.0f && std::abs(driving.aside) < 1.0f);
}

TEST("vehicles: a helicopter spinning out goes on to its crash speed and spins up; the others go on as they went")
{
    const auto& hind = Killstreaks::Get(Killstreak::Gunship);
    // from 800 u/s on x, slowing to its crash speed (55 mph) at its acceleration, spinning up over 3 s
    const auto motion = [&](glm::vec3 velocity, float seconds) {
        return Vehicles::GetDeathMotion(hind, Killstreaks::MOTION_SPINOUT, velocity, seconds);
    };
    const auto start = motion(glm::vec3(800.0f, 0.0f, 0.0f), 0.0f);
    CHECK(glm::length(start.offset) == 0.0f && start.yaw == 0.0f);
    const auto speedAt = [&](float seconds) {
        return (motion(glm::vec3(800.0f, 0, 0), seconds + 0.01f).offset.x -
                motion(glm::vec3(800.0f, 0, 0), seconds).offset.x) /
               0.01f;
    };
    CHECK_NEAR(speedAt(0.0f), 800.0f, 5.0f);
    CHECK_NEAR(speedAt(5.0f), hind.crashSpeed, 1.0f);
    CHECK_NEAR(motion(glm::vec3(0.0f), 1.5f).yaw, hind.spinRate * 1.5f * 1.5f / 6.0f, 1e-3);
    CHECK_NEAR(motion(glm::vec3(0.0f), 4.0f).yaw, hind.spinRate * 2.5f, 1e-3);
    // its climb stops over a second
    CHECK_NEAR(motion(glm::vec3(0.0f, 0.0f, 100.0f), 2.0f).offset.z, 50.0f, 1e-3);
    // an explosion goes on as it went, not turning
    const auto blown = Vehicles::GetDeathMotion(COBRA, Killstreaks::MOTION_EXPLODE, glm::vec3(700.0f, 0, 10.0f), 0.1f);
    CHECK(glm::distance(blown.offset, glm::vec3(70.0f, 0.0f, 1.0f)) < 1e-3 && blown.yaw == 0.0f);
}

TEST("vehicles: a click picks a killstreak by its body, below a helicopter's rotor")
{
    // the Cobra's body hangs under its origin (its rotor): a ray at its middle picks it, one well above its rotor
    // doesn't
    const auto sphere = Killstreaks::GetPickSphere(COBRA);
    const glm::vec3 origin(0.0f, 0.0f, 1000.0f), center = origin + glm::vec3(0.0f, 0.0f, sphere.up);
    const glm::vec3 camera(0.0f, -1500.0f, 950.0f);
    CHECK(ActorLogic::RayHitsSphere(camera, glm::normalize(origin + glm::vec3(0, 0, -60) - camera), center,
                                    sphere.radius));
    CHECK(!ActorLogic::RayHitsSphere(camera, glm::normalize(origin + glm::vec3(0, 0, 400) - camera), center,
                                     sphere.radius));
    // (where it enters it, and nothing behind the camera)
    const auto hit = ActorLogic::RayHitsSphere(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(100, 0, 0), 10);
    CHECK(hit && std::abs(*hit - 90.0f) < 1e-3f);
    CHECK(!ActorLogic::RayHitsSphere(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-100, 0, 0), 10));
    CHECK(Killstreaks::GetPickSphere(RCXD).radius < 30.0f);
}

// A loop of 2026-10-30: a Cobra added as the Actors tab adds it, three
// points of its path added from the view's menu (Add point here), which left its 1 s starting Fly as it was

namespace
{
    Types::Actor MakeOwnerLoop()
    {
        auto actor = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 18784, 60000, -167.5f);
        actor.path = {glm::vec3(141.83f, 821.01f, 1228.0f), glm::vec3(-408.10f, -229.33f, 1231.53f),
                      glm::vec3(892.08f, -923.90f, 1228.0f), glm::vec3(1150.32f, 654.40f, 1228.0f)};
        return actor;
    }

    // the most its yaw, pitch and roll change between 16 ms frames from one tick to another
    struct Turns
    {
        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
    };
    Turns GetTurns(const Types::Actor& actor, const Killstreaks::Info& info, std::uint32_t from, std::uint32_t to)
    {
        const auto animations = Animations();
        Turns turns;
        std::optional<std::tuple<float, float, float>> last;
        for (auto tick = from; tick < to; tick += 16)
        {
            const auto yaw = ActorLogic::EvaluatePlacement(actor, tick, animations).yaw;
            const auto body = Vehicles::GetBody(info, tick, PlacementOf(actor),
                                                ActorLogic::GetPathProgress(actor, tick, animations), actor.id);
            if (last)
            {
                const auto [lastYaw, lastPitch, lastRoll] = *last;
                turns.yaw = std::max(turns.yaw, std::abs(std::remainder(yaw - lastYaw, 360.0f)));
                turns.pitch = std::max(turns.pitch, std::abs(body.pitch - lastPitch));
                turns.roll = std::max(turns.roll, std::abs(body.roll - lastRoll));
            }
            last = {yaw, body.pitch, body.roll};
        }
        return turns;
    }
}  // namespace

TEST("vehicles: a flight's clips too short for its path never fly it faster than the killstreak goes")
{
    // the loop as its scene had it: 3,535 units in a 1 s Fly and the Hover's 3.5 s slow-down, which flew it
    // at 1,622 u/s (2.3 times its cruise) at 8.6 times its acceleration; now no faster than its fastest preset (60 mph,
    // evading), at rest short of the end
    const auto animations = Animations();
    const auto actor = MakeOwnerLoop();
    const auto rest = actor.clips[1].start + actor.clips[1].blendIn;
    const auto steps = GetSteps(actor, COBRA, 18000, rest + 500);
    CHECK(steps.fastest <= Killstreaks::GetTopSpeed(COBRA) + 1.0f);
    CHECK(steps.speed < 25.0f);
    CHECK(SpeedOf(actor, rest) < 0.5f);
    CHECK(ActorLogic::GetPathProgress(actor, rest, animations) < ActorLogic::GetPathLength(actor, animations) - 100.0f);
    CHECK(Killstreaks::GetTopSpeed(COBRA) == Killstreaks::FromMph(60.0f));
}

TEST("vehicles: the owner's loop fitted again to its path flies it at its cruise, its yaw, pitch and roll smooth")
{
    // a change of its path fits its flight again (as placing its points does): speeding up over 3.5 s, cruising, slowing
    // to rest at its end, its corners at no more than a 1.3 degree turn, a degree of pitch and 1.25 of roll a frame
    const auto animations = Animations();
    auto actor = MakeOwnerLoop();
    ActorLogic::FitOnlyWalk(actor, animations);
    const auto& fly = actor.clips[0];
    const auto& hover = actor.clips[1];
    CHECK(fly.length > 4000 && fly.blendIn == 3500 && hover.start == fly.End() && hover.blendIn == 3500);
    const auto rest = hover.start + hover.blendIn;
    const auto steps = GetSteps(actor, COBRA, 18000, rest + 500);
    CHECK_NEAR(steps.fastest, Killstreaks::GetCruiseSpeed(COBRA), 2.0f);
    CHECK(steps.speed < 10.0f);
    const auto turns = GetTurns(actor, COBRA, 18000, rest + 500);
    CHECK(turns.yaw < 1.3f);
    CHECK(turns.pitch < 1.0f);
    CHECK(turns.roll < 1.25f);
    CHECK(glm::distance(ActorLogic::EvaluatePlacement(actor, rest, animations).position, actor.path.back()) < 0.5f);
}

TEST("vehicles: Fly here in a corner sets off from exactly where the helicopter is, the arc before it as it was flown")
{
    // the loop fitted, told to fly elsewhere halfway round its first corner (turning left): its way up to then
    // as it was, a stop where it was (a point of its path), its turn and bank smooth through the tick
    const auto animations = Animations();
    auto loop = MakeOwnerLoop();
    ActorLogic::FitOnlyWalk(loop, animations);
    const auto rounded = ActorLogic::GetRoundedPath(loop, animations);
    const auto arc = std::ranges::find_if(rounded.pieces, [](const auto& piece) { return piece.radius > 0.0f; });
    CHECK(arc != rounded.pieces.end());
    const auto halfway = arc->distance + arc->length / 2.0f;
    auto tick = loop.clips[0].start;
    while (ActorLogic::GetPathProgress(loop, tick, animations) < halfway)
        tick += 10;
    std::vector<Types::ActorPlacement> was;
    for (auto at = loop.clips[0].start; at <= tick; at += 50)
        was.push_back(ActorLogic::EvaluatePlacement(loop, at, animations));
    const auto there = ActorLogic::EvaluatePlacement(loop, tick, animations);
    const auto flyHere = [&](glm::vec3 target) {
        auto actor = loop;
        const auto fly = std::string(Killstreaks::MOTION_FLY), hover = std::string(Killstreaks::MOTION_HOVER);
        CHECK(ActorLogic::WalkTo(actor, tick, target, fly, hover, animations, actor.pathSpeed) != 0);
        std::size_t i = 0;
        for (auto at = actor.clips[0].start; at <= tick; at += 50, ++i)
        {
            const auto now = ActorLogic::EvaluatePlacement(actor, at, animations);
            CHECK(glm::distance(now.position, was[i].position) < 0.5f);
            CHECK(std::abs(std::remainder(now.yaw - was[i].yaw, 360.0f)) < 0.1f);
        }
        // (the stop: the point given twice, where it was; the corner's point moved back along the way it came)
        const auto stop = std::ranges::adjacent_find(actor.path);
        CHECK(stop != actor.path.end() && glm::distance(*stop, there.position) < 0.5f);
        CHECK(glm::distance(actor.path[1], loop.path[1]) > 50.0f);
        const auto& rest = actor.clips.back();
        CHECK(glm::distance(ActorLogic::EvaluatePlacement(actor, rest.start + rest.blendIn, animations).position,
                            target) < 0.5f);
        // (the path bar: the moved corner passed at the tick, where its arc ends, and the arrival as it comes to rest)
        const auto timing = ActorLogic::GetPathTiming(actor, animations);
        CHECK(timing.reached[1] && *timing.reached[1] + 10 >= tick && *timing.reached[1] <= tick + 10);
        CHECK(timing.arrival && *timing.arrival + 100 >= rest.start + rest.blendIn &&
              *timing.arrival <= rest.start + rest.blendIn);
        return GetTurns(actor, COBRA, tick - 1000, tick + 6000);
    };
    // on round the way it turned
    const auto on = flyHere(glm::vec3(1500.0f, -1500.0f, 1300.0f));
    CHECK(on.yaw < 1.3f);
    CHECK(on.pitch < 1.0f);
    CHECK(on.roll < 1.25f);
    // back the other way: it rolls from its bank to the other side's over half a second (on round the corner first, as
    // before, rolled at up to 3.9 degrees a frame)
    const auto back = flyHere(glm::vec3(-1500.0f, -1500.0f, 1300.0f));
    CHECK(back.yaw < 1.3f);
    CHECK(back.pitch < 1.0f);
    CHECK(back.roll < 2.5f);
}

TEST("vehicles: a flight comes to rest at its path's end, where the path bar has it arrive")
{
    // the short path of actors-killstreak-points, Fly here at its first corner (in game the path bar had it arrive at
    // 71 s: the speed solved for the way, from the way a speed of 1 u/s adds against 2,357 units, came out 0.013 units
    // short of the end, and arrival is within 0.01)
    const auto animations = Animations();
    auto actor = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 20000, 87808, 0.0f);
    actor.path = {glm::vec3(0.0f, -800.0f, 1006.0f), glm::vec3(700.0f, -800.0f, 1006.0f),
                  glm::vec3(700.0f, -100.0f, 1006.0f), glm::vec3(-300.0f, 0.0f, 1006.0f)};
    ActorLogic::FitOnlyWalk(actor, animations);
    const auto fly = std::string(Killstreaks::MOTION_FLY), hover = std::string(Killstreaks::MOTION_HOVER);
    ActorLogic::WalkTo(actor, 22794, glm::vec3(-200.0f, 600.0f, 1038.9f), fly, hover, animations, actor.pathSpeed);
    const auto& rest = actor.clips.back();
    const auto length = ActorLogic::GetPathLength(actor, animations);
    for (const auto tick : {rest.start + rest.blendIn, 30000u, 60000u})
        CHECK(std::abs(ActorLogic::GetPathProgress(actor, tick, animations) - length) < 0.005f);
    const auto timing = ActorLogic::GetPathTiming(actor, animations);
    CHECK(timing.arrival && *timing.arrival + 100 >= rest.start + rest.blendIn &&
          *timing.arrival <= rest.start + rest.blendIn);
}

TEST("vehicles: a helicopter banks round the tight corners of a short path without pitching")
{
    // four points added from the view's menu on the dirt field (actors-killstreak-points): corners of 240 and 515 units
    // flown at up to 650 u/s, turning 100 degrees a second. Its acceleration measured in its own axes as it faced then:
    // the pull round a corner is to its side (with the world's axes, the pull from before the tick and after it came out
    // partly ahead, and its nose swung 1.4 degrees a frame); it banks at up to 75 degrees a second
    const auto animations = Animations();
    auto actor = Killstreaks::MakeStart(Killstreak::AttackHelicopter, 20000, 87808, 0.0f);
    actor.path = {glm::vec3(0.0f, -800.0f, 1006.0f), glm::vec3(700.0f, -800.0f, 1006.0f),
                  glm::vec3(700.0f, -100.0f, 1006.0f), glm::vec3(-300.0f, 0.0f, 1006.0f)};
    ActorLogic::FitOnlyWalk(actor, animations);
    const auto rest = actor.clips[1].start + actor.clips[1].blendIn;
    const auto turns = GetTurns(actor, COBRA, 19500, rest + 500);
    CHECK(turns.pitch < 1.0f);
    CHECK(turns.roll < 1.25f);
    CHECK(turns.yaw > 1.0f);
    // round a steady circle: banked as far as the pull takes it, its nose as its speed puts it, steady
    const auto circling = Vehicles::GetBody(COBRA, 5000, Circle(600.0f, 500.0f), 0.0f, 1);
    CHECK_NEAR(circling.pitch, COBRA.maxPitch * 0.5f * 500.0f / Killstreaks::GetTopSpeed(COBRA), 0.3f);
    CHECK(circling.roll < -20.0f);
    CHECK(std::abs(circling.ahead) < 5.0f);
}
