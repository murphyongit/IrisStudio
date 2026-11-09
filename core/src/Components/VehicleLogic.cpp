#include "StdInclude.hpp"
#include "VehicleLogic.hpp"

namespace IWXMVM::Components::VehicleLogic
{
    using Types::VehicleClass;

    namespace
    {
        // a velocity is measured between placements at least this many milliseconds apart
        constexpr std::uint32_t VELOCITY_STEP = 10;
        // and over about this many steps across the tilt time's window, both sides
        constexpr float WINDOW_STEPS = 40.0f;
        // and a car's steering by how it turns between this many before and after it (its steering is quick: about
        // 0.15 s from straight to full lock, nitrous_steer_speed)
        constexpr std::uint32_t STEER_STEP = 100;

        // The hover drift: slow waves of a few seconds, out of step with each other, each a share of the drift, so it
        // stays within it (the x and y waves come to 0.99 of it at most in a diagonal); the rise and sink about 3/4 of
        // it (the game's Attack Helicopter bobbed about 37 units with its 50)
        struct Wave
        {
            float period = 1.0f;  // seconds
            float share = 0.0f;
        };
        constexpr std::array<Wave, 2> DRIFT_X{{{7.3f, 0.45f}, {4.7f, 0.25f}}};
        constexpr std::array<Wave, 2> DRIFT_Y{{{8.9f, 0.45f}, {5.3f, 0.25f}}};
        constexpr Wave DRIFT_Z{6.1f, 0.74f};
        // and it rocks with the drift: degrees of roll for each unit a second it drifts to its side (the game's
        // helicopters tilt from their sideways speed, tiltFromVelocityRoll 20 in heli_ai_mp: measured, about 11 degrees
        // at most as the Attack Helicopter drifted)
        constexpr float HOVER_ROLL = 0.28f;

        float Clamp(float value, float most)
        {
            return std::clamp(value, -most, most);
        }

        // a tilt within its most either side, easing into it: as it is up to EASED_FROM of its most, then nearer its most
        // the further it is pushed (tanh), so it comes up to its most smoothly instead of stopping there at once
        constexpr float EASED_FROM = 0.8f;
        float EaseWithin(float value, float most)
        {
            const auto knee = EASED_FROM * most;
            if (most <= 0.0f || std::abs(value) <= knee)
                return Clamp(value, most);
            const auto room = most - knee;
            return std::copysign(knee + room * std::tanh((std::abs(value) - knee) / room), value);
        }

        float Degrees(float radians)
        {
            return glm::degrees(radians);
        }

        // the phase of one of an actor's drift's waves (0 to 2 pi), the same for the same seed and wave
        float GetPhase(std::uint32_t seed, std::uint32_t wave)
        {
            auto hash = seed * 2654435761u + (wave + 1) * 40503u;
            hash ^= hash >> 15;
            hash *= 2246822519u;
            hash ^= hash >> 13;
            return static_cast<float>(hash % 10000u) / 10000.0f * glm::two_pi<float>();
        }

        // a wave's offset at a time, its velocity and its acceleration
        struct WaveAt
        {
            float offset = 0.0f;
            float velocity = 0.0f;
            float acceleration = 0.0f;
        };
        WaveAt GetWave(const Wave& wave, float radius, float phase, float seconds)
        {
            const auto rate = glm::two_pi<float>() / wave.period;
            const auto value = std::sin(rate * seconds + phase);
            return {radius * wave.share * value, radius * wave.share * rate * std::cos(rate * seconds + phase),
                    -radius * wave.share * rate * rate * value};
        }

        // How it moves about a tick: its velocity and acceleration averaged over the window either side, in its own axes
        // (x the way it faces, y its left, z up) as it faced at each moment of it
        struct Motion
        {
            glm::vec3 velocity{0.0f};
            glm::vec3 acceleration{0.0f};
            float yaw = 0.0f;  // at the tick
        };

        // a vector of the world in a vehicle's axes as it faces a yaw (degrees)
        glm::dvec3 ToOwnAxes(const glm::dvec3& vector, double yaw)
        {
            const auto radians = glm::radians(yaw);
            const auto c = std::cos(radians), s = std::sin(radians);
            return {c * vector.x + s * vector.y, -s * vector.x + c * vector.y, vector.z};
        }

        // how far it turns to the left (degrees) for how far it goes forward between two placements (along the arc it
        // goes round: on a circle the line across it is shorter than the arc)
        std::pair<float, float> GetTurn(const Types::ActorPlacement& from, const Types::ActorPlacement& to, float yaw)
        {
            const auto turned = std::remainder(to.yaw - from.yaw, 360.0f);
            const glm::vec3 forward(std::cos(glm::radians(yaw)), std::sin(glm::radians(yaw)), 0.0f);
            const auto half = glm::radians(std::abs(turned)) / 2.0f;
            return {turned, glm::dot(to.position - from.position, forward) * (half > 1e-4f ? half / std::sin(half) : 1.0f)};
        }

        // Its velocity and acceleration about a tick, averaged over a window either side weighed as a bell (Hann: most
        // at the tick, none at its edges), so a change of speed enters and leaves the average smoothly instead of all
        // at once. Measured between placements on a grid of whole steps, the same whatever the tick, so the average
        // changes smoothly as the tick moves: the velocity between two of them weighed by as much of the window as
        // lies between them, and the acceleration by parts (each change of velocity at a placement weighed by the
        // window there), so steady motion has none. Each in its own axes as it faced then, as a vehicle feels its
        // acceleration: round a corner at a steady speed the pull is all to its side, so it banks and doesn't pitch
        // (averaged in the world's axes, the pull from before and after the tick, turned with the corner, came out
        // partly ahead or behind).
        Motion Measure(std::uint32_t tick, float window, const PlacementAt& placementAt)
        {
            Motion motion;
            motion.yaw = placementAt(tick).yaw;
            const auto half = static_cast<double>(std::max(window, 0.05f));
            const auto step = std::max(static_cast<double>(VELOCITY_STEP) / 1000.0,
                                       std::round(2000.0 * half / WINDOW_STEPS) / 1000.0);
            const auto now = static_cast<double>(tick) / 1000.0;
            const auto first = static_cast<std::int64_t>(std::floor((now - half) / step));
            const auto last = static_cast<std::int64_t>(std::ceil((now + half) / step));
            const auto at = [&](std::int64_t point) {
                const auto seconds = std::max(static_cast<double>(point) * step, 0.0);
                return placementAt(static_cast<std::uint32_t>(std::lround(seconds * 1000.0)));
            };
            // the window's weight at an offset from the tick, and its integral from its start
            const auto rate = glm::pi<double>() / half;
            const auto weight = [&](double offset) {
                return std::abs(offset) >= half ? 0.0 : 0.5 + 0.5 * std::cos(rate * offset);
            };
            const auto area = [&](double offset) {
                offset = std::clamp(offset, -half, half);
                return 0.5 * (offset + half) + 0.5 / rate * std::sin(rate * offset);
            };
            auto before = at(first);
            glm::dvec3 velocity{0.0}, acceleration{0.0}, was{0.0};
            for (auto point = first; point < last; ++point)
            {
                const auto from = static_cast<double>(point) * step - now;
                const auto after = at(point + 1);
                const auto moved = glm::dvec3(after.position - before.position) / step;
                // (the velocity as it faced between the two, the change of it as it faced at the placement)
                const auto turned = std::remainder(static_cast<double>(after.yaw) - before.yaw, 360.0);
                const auto between = before.yaw + turned / 2.0;
                velocity += ToOwnAxes(moved, between) * (area(from + step) - area(from));
                if (point > first)
                    acceleration += ToOwnAxes(moved - was, before.yaw) * weight(from);
                before = after;
                was = moved;
            }
            motion.velocity = glm::vec3(velocity / half);
            motion.acceleration = glm::vec3(acceleration / half);
            return motion;
        }

        float GetFastest(const KillstreakLogic::Info& info)
        {
            return std::max(KillstreakLogic::GetTopSpeed(info), 1.0f);
        }
    }  // namespace

    float GetRampTime(float from, float to, float acceleration)
    {
        const auto change = std::abs(to - from);
        if (acceleration <= 0.0f || change <= 0.0f)
            return 0.0f;
        const auto time = change / acceleration;
        return time + std::min(RAMP_CORNER, time);
    }

    std::uint32_t GetRampTicks(float from, float to, float acceleration)
    {
        return static_cast<std::uint32_t>(std::lround(GetRampTime(from, to, acceleration) * 1000.0f));
    }

    // (the acceleration rises over the corner, holds, and falls over the corner: the share made goes up as a parabola,
    // a line, a parabola)
    float GetRampShare(float seconds, float time)
    {
        if (seconds <= 0.0f)
            return 0.0f;
        if (seconds >= time)
            return 1.0f;
        const auto corner = std::min(RAMP_CORNER, time / 2.0f);
        const auto rate = 1.0f / (time - corner);
        if (seconds < corner)
            return rate * seconds * seconds / (2.0f * corner);
        if (seconds <= time - corner)
            return rate * (seconds - corner / 2.0f);
        const auto left = time - seconds;
        return 1.0f - rate * left * left / (2.0f * corner);
    }

    float GetRampArea(float seconds, float time)
    {
        if (seconds <= 0.0f)
            return 0.0f;
        if (seconds >= time)
            return time / 2.0f + (seconds - time);
        const auto corner = std::min(RAMP_CORNER, time / 2.0f);
        const auto rate = 1.0f / (time - corner);
        if (seconds < corner)
            return rate * seconds * seconds * seconds / (6.0f * corner);
        if (seconds <= time - corner)
        {
            const auto past = seconds - corner / 2.0f;
            return rate * corner * corner / 6.0f + rate * (past * past - corner * corner / 4.0f) / 2.0f;
        }
        const auto left = time - seconds;
        return time / 2.0f - left + rate * left * left * left / (6.0f * corner);
    }

    namespace
    {
        // a piece's speed and how far along the path it has got some ticks into it (within it)
        float GetPieceSpeed(const Profile::Piece& piece, std::uint32_t into)
        {
            if (into >= piece.ramp)
                return piece.to;
            return piece.from + (piece.to - piece.from) * GetRampShare(static_cast<float>(into) / 1000.0f,
                                                                       static_cast<float>(piece.ramp) / 1000.0f);
        }

        float GetPieceDistance(const Profile::Piece& piece, std::uint32_t into)
        {
            const auto seconds = static_cast<float>(into) / 1000.0f;
            return piece.distance + piece.from * seconds +
                   (piece.to - piece.from) * GetRampArea(seconds, static_cast<float>(piece.ramp) / 1000.0f);
        }

        // a piece's start, from where the one before left it: going on at its speed, or standing since it stopped
        void FollowOn(std::vector<Profile::Piece>& pieces, std::size_t index)
        {
            auto& piece = pieces[index];
            if (index == 0)
            {
                piece.from = 0.0f;
                piece.distance = 0.0f;
                return;
            }
            const auto& before = pieces[index - 1];
            const auto into = std::min(piece.start, before.end) - before.start;
            piece.distance = GetPieceDistance(before, into);
            piece.from = before.end < piece.start ? 0.0f : GetPieceSpeed(before, into);
        }

        // which piece is shown at a tick: the last started by then
        const Profile::Piece* FindPiece(const Profile& profile, std::uint32_t tick)
        {
            const auto after = std::ranges::upper_bound(profile.pieces, tick, {}, &Profile::Piece::start);
            return after == profile.pieces.begin() ? nullptr : &*std::prev(after);
        }
    }  // namespace

    float Profile::Distance(std::uint32_t tick) const
    {
        const auto piece = FindPiece(*this, tick);
        return piece ? GetPieceDistance(*piece, std::min(tick, piece->end) - piece->start) : 0.0f;
    }

    float Profile::Speed(std::uint32_t tick) const
    {
        const auto piece = FindPiece(*this, tick);
        return piece && tick < piece->end ? GetPieceSpeed(*piece, tick - piece->start) : 0.0f;
    }

    Profile GetProfile(std::span<const Leg> legs, float end, float fastest)
    {
        Profile profile;
        auto& pieces = profile.pieces;
        for (const auto& leg : legs)
            pieces.push_back({.start = leg.start, .end = leg.end, .ramp = leg.blend, .to = leg.speed.value_or(0.0f)});
        // (each goes until the next starts, or stands from its own end until then)
        for (std::size_t i = 0; i + 1 < pieces.size(); ++i)
            pieces[i].end = std::max(std::min(pieces[i].end, pieces[i + 1].start), pieces[i].start);

        // the last moving piece of the flight going on, if one is
        std::optional<std::size_t> moving;
        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
            FollowOn(pieces, i);
            if (legs[i].speed)
            {
                moving = i;
                continue;
            }
            if (!moving)
                continue;
            // a flight stopped where its clip did isn't one that comes to rest, nor one still slowing as the next
            // starts
            auto& piece = pieces[i];
            const bool stood = pieces[i - 1].end < piece.start;
            if (!stood && static_cast<std::uint64_t>(piece.start) + piece.ramp > piece.end)
                continue;
            const bool last = std::none_of(legs.begin() + static_cast<std::ptrdiff_t>(i) + 1, legs.end(),
                                           [](const Leg& leg) { return leg.speed.has_value(); });
            if (!stood && last && !legs[i].dies)
            {
                // where it comes to rest goes as the speed of its last moving piece: as fast as gets it there, but no
                // faster than it goes, or than its clip says (clips too short for the way stop it short of the end)
                const auto restsAt = [&](float speed) {
                    pieces[*moving].to = speed;
                    for (auto k = *moving + 1; k <= i; ++k)
                        FollowOn(pieces, k);
                    return GetPieceDistance(piece, piece.ramp);
                };
                const auto own = legs[*moving].speed.value_or(0.0f);
                // (the way it goes for each unit a second, measured over its own speed, and the speed found corrected
                // once from where it gets: the sums lose a little, which was enough to leave it short of the end)
                const auto probe = std::max(own, 1.0f);
                const auto still = restsAt(0.0f), more = (restsAt(probe) - still) / probe;
                auto speed = own;
                if (more > 1e-4f)
                {
                    speed = std::max((end - still) / more, 0.0f);
                    speed = std::max(speed + (end - restsAt(speed)) / more, 0.0f);
                }
                restsAt(std::min(speed, std::max(fastest, own)));
            }
            moving.reset();
        }
        return profile;
    }

    FlightFit FitFlight(float distance, float from, float speed, float acceleration)
    {
        FlightFit fit;
        distance = std::max(distance, 0.0f);
        from = std::max(from, 0.0f);
        const auto ticks = [](float seconds) {
            return static_cast<std::uint32_t>(std::lround(std::max(seconds, 0.0f) * 1000.0f));
        };
        if (acceleration <= 0.0f || speed <= 0.0f)
        {
            fit.peak = speed;
            fit.length = speed > 0.0f ? ticks(distance / speed) : 0;
            return fit;
        }
        // the way a flight to a peak takes, speeding up to it and slowing from it
        const auto way = [&](float peak) {
            return GetRampTime(from, peak, acceleration) * (from + peak) / 2.0f +
                   GetRampTime(peak, 0.0f, acceleration) * peak / 2.0f;
        };
        float hold = 0.0f;
        if (way(speed) <= distance)
        {
            fit.peak = speed;
            hold = (distance - way(speed)) / speed;
        }
        else if (from > 0.0f && way(from) >= distance)
        {
            // going too fast to stop there at its acceleration: slowing from where it is to rest over the way left
            fit.peak = from;
            fit.slowDown = ticks(2.0f * distance / from);
            return fit;
        }
        else
        {
            // a short way: as fast as it gets before it has to slow down
            float low = from, high = speed;
            for (int i = 0; i < 40; ++i)
                (way((low + high) / 2.0f) < distance ? low : high) = (low + high) / 2.0f;
            fit.peak = (low + high) / 2.0f;
        }
        fit.speedUp = GetRampTicks(from, fit.peak, acceleration);
        fit.length = fit.speedUp + ticks(hold);
        fit.slowDown = GetRampTicks(fit.peak, 0.0f, acceleration);
        return fit;
    }

    Body GetBody(const KillstreakLogic::Info& info, std::uint32_t tick, const PlacementAt& placementAt, float distance,
                 std::uint32_t seed)
    {
        Body body;
        if (info.vehicleClass != VehicleClass::Helicopter && info.vehicleClass != VehicleClass::Plane &&
            info.vehicleClass != VehicleClass::Car)
            return body;

        const auto motion = Measure(tick, info.tiltTime, placementAt);
        const glm::vec3 forward(std::cos(glm::radians(motion.yaw)), std::sin(glm::radians(motion.yaw)), 0.0f);
        const glm::vec3 left(-forward.y, forward.x, 0.0f);
        const auto ahead = motion.acceleration.x;
        const auto aside = motion.acceleration.y;
        body.speed = motion.velocity.x;
        body.ahead = ahead;
        body.aside = aside;

        switch (info.vehicleClass)
        {
            case VehicleClass::Helicopter:
            {
                // half its most pitch at its fastest, half its most roll going sideways that fast, and the tilt that
                // makes its rotor's thrust push it as it speeds up and turns
                const auto fastest = GetFastest(info);
                auto pitch = info.maxPitch * 0.5f * Clamp(motion.velocity.x / fastest, 1.0f) +
                             Degrees(std::atan2(ahead, GRAVITY));
                auto roll = -(info.maxRoll * 0.5f * Clamp(motion.velocity.y / fastest, 1.0f) +
                              Degrees(std::atan2(aside, GRAVITY)));

                // hovering: drifting about where it is, tilting with it, rocking with its drift to its side
                const auto speed = glm::length(glm::vec2(motion.velocity));
                const auto slow = std::clamp(1.0f - speed / HOVER_SPEED, 0.0f, 1.0f);
                const auto weight = slow * slow * (3.0f - 2.0f * slow);
                if (weight > 0.0f && info.hoverDrift > 0.0f)
                {
                    const auto seconds = static_cast<float>(tick) / 1000.0f;
                    glm::vec3 drift{0.0f}, drifting{0.0f}, pushed{0.0f};
                    for (std::uint32_t i = 0; i < DRIFT_X.size(); ++i)
                    {
                        const auto x = GetWave(DRIFT_X[i], info.hoverDrift, GetPhase(seed, i), seconds);
                        const auto y = GetWave(DRIFT_Y[i], info.hoverDrift, GetPhase(seed, i + 2), seconds);
                        drift += glm::vec3(x.offset, y.offset, 0.0f);
                        drifting += glm::vec3(x.velocity, y.velocity, 0.0f);
                        pushed += glm::vec3(x.acceleration, y.acceleration, 0.0f);
                    }
                    drift.z = GetWave(DRIFT_Z, info.hoverDrift, GetPhase(seed, 4), seconds).offset;
                    body.drift = drift * weight;
                    pitch += weight * Degrees(std::atan2(glm::dot(pushed, forward), GRAVITY));
                    roll -= weight * (Degrees(std::atan2(glm::dot(pushed, left), GRAVITY)) +
                                      HOVER_ROLL * glm::dot(drifting, left));
                }
                body.pitch = EaseWithin(pitch, info.maxPitch);
                body.roll = EaseWithin(roll, info.maxRoll);
                break;
            }
            case VehicleClass::Plane:
            {
                const auto across = std::max(glm::length(glm::vec2(motion.velocity)), 1.0f);
                body.pitch = Clamp(-Degrees(std::atan2(motion.velocity.z, across)), info.maxPitch);
                body.roll = Clamp(-Degrees(std::atan2(aside, GRAVITY)), info.maxRoll);
                break;
            }
            case VehicleClass::Car:
            {
                const auto& wheels = info.wheels;
                // squatting at twice its acceleration at most, leaning at the most its tyres hold
                if (info.acceleration > 0.0f)
                    body.bodyPitch = -info.maxPitch * Clamp(ahead / (2.0f * info.acceleration), 1.0f);
                if (wheels.grip > 0.0f)
                    body.bodyRoll = info.maxRoll * Clamp(aside / (wheels.grip * GRAVITY), 1.0f);
                // the steering that turns it as much as it turns for the distance it goes about the tick
                const auto [turned, gone] = GetTurn(placementAt(tick > STEER_STEP ? tick - STEER_STEP : 0),
                                                    placementAt(tick + STEER_STEP), motion.yaw);
                if (std::abs(gone) > 0.5f)
                    body.steer = Clamp(Degrees(std::atan((wheels.front + wheels.back) * glm::radians(turned) / gone)),
                                       wheels.maxSteer);
                if (wheels.radius > 0.0f)
                    body.wheelTurn = std::fmod(Degrees(distance / wheels.radius), 360.0f);
                break;
            }
            default:
                break;
        }
        return body;
    }

    bool IsDustTick(std::uint32_t tick, float height)
    {
        if (height > DUST_HIGHEST || tick % DUST_STEP != 0)
            return false;
        // (the game waits from 0.05 s at its lowest to 0.15 s at its highest, never less than its step)
        const auto wait = std::clamp((height - DUST_LOWEST) / (DUST_HIGHEST - DUST_LOWEST), 0.0f, 1.0f) * 0.1f + 0.05f;
        const auto every =
            std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(wait * 1000.0f / DUST_STEP)));
        return tick / DUST_STEP % every == 0;
    }

    std::vector<Tread> GetTreads(const KillstreakLogic::Info& info, const Body& body)
    {
        std::vector<Tread> treads;
        // (as fast as its fastest preset: the definition's top speed)
        const auto top = KillstreakLogic::GetTopSpeed(info);
        if (info.vehicleClass != VehicleClass::Car || top <= 0.0f)
            return treads;
        constexpr float SLOW = 5.0f;  // units a second
        const auto fraction = std::abs(body.speed) / top;
        bool trail = false;
        if (body.ahead >= 0.75f * info.acceleration && fraction < 0.5f && body.speed > SLOW)
        {
            treads.push_back(Tread::Peel);
            trail = true;
        }
        if (info.wheels.grip > 0.0f && std::abs(body.aside) >= 0.75f * info.wheels.grip * GRAVITY)
        {
            treads.push_back(Tread::Drift);
            trail = true;
        }
        if (body.speed < -SLOW)
            treads.push_back(Tread::Reverse);
        else if (fraction >= 0.25f)
            treads.push_back(Tread::Driving);
        else if (body.speed > SLOW)
            treads.push_back(Tread::Slow);
        if (trail || (fraction >= 0.75f && body.speed > 0.0f))
            treads.push_back(Tread::Trail);
        return treads;
    }

    std::string GetTreadEffect(std::string_view surface, Tread tread)
    {
        constexpr std::array<std::string_view, 6> SUFFIXES{"", "_slow", "_reverse", "_trail", "_drift", "_peel"};
        return std::format("vehicle/treadfx/fx_treadfx_rcbomb_{}{}", surface,
                           SUFFIXES[static_cast<std::size_t>(tread)]);
    }

    DeathMotion GetDeathMotion(const KillstreakLogic::Info& info, std::string_view motion, glm::vec3 velocity,
                               float seconds)
    {
        DeathMotion death;
        seconds = std::max(seconds, 0.0f);
        if (motion != KillstreakLogic::MOTION_SPINOUT)
        {
            death.offset = velocity * seconds;
            return death;
        }
        // along the way it went, from its speed to its crash speed at its acceleration
        const auto across = glm::vec2(velocity);
        const auto speed = glm::length(across);
        const auto way = speed > 1e-3f ? across / speed : glm::vec2(0.0f);
        const auto change = info.crashSpeed - speed;
        const auto time = info.acceleration > 0.0f ? std::abs(change) / info.acceleration : 0.0f;
        const auto changing = std::min(seconds, time);
        const auto rate = time > 0.0f ? change / time : 0.0f;
        const auto gone = speed * changing + rate * changing * changing / 2.0f + info.crashSpeed * (seconds - changing);
        // (with no way yet, as a hover's: on the way it faces is the game's, which this has no way to know: it stays)
        death.offset = glm::vec3(way * gone, 0.0f);
        const auto falling = std::min(seconds, 1.0f);
        death.offset.z = velocity.z * (falling - falling * falling / 2.0f);
        // spinning up over SPIN_OUT_TIME
        const auto up = std::min(seconds, SPIN_OUT_TIME);
        death.yaw = info.spinRate * (up * up / (2.0f * SPIN_OUT_TIME) + (seconds - up));
        return death;
    }

    std::vector<Joint> GetJoints(const KillstreakLogic::Info& info, const Body& body)
    {
        std::vector<Joint> joints;
        if (info.vehicleClass != VehicleClass::Car)
            return joints;
        const auto& wheels = info.wheels;
        if (!wheels.body.empty())
            joints.push_back({wheels.body, {body.bodyPitch, 0.0f, body.bodyRoll}});
        for (const auto bone : wheels.steer)
            joints.push_back({bone, {0.0f, body.steer, 0.0f}});
        for (const auto bone : wheels.roll)
            joints.push_back({bone, {body.wheelTurn, 0.0f, 0.0f}});
        return joints;
    }

    float GetTurnRadius(const KillstreakLogic::Info& info, float speed)
    {
        auto radius = 0.0f;
        switch (info.vehicleClass)
        {
            case VehicleClass::Helicopter:
            case VehicleClass::Plane:
                if (info.maxRoll > 0.0f)
                    radius = speed * speed / (GRAVITY * std::tan(glm::radians(info.maxRoll)));
                break;
            case VehicleClass::Car:
            {
                const auto& wheels = info.wheels;
                if (wheels.maxSteer > 0.0f)
                    radius = (wheels.front + wheels.back) / std::tan(glm::radians(wheels.maxSteer));
                if (wheels.grip > 0.0f)
                    radius = std::max(radius, speed * speed / (wheels.grip * GRAVITY));
                break;
            }
            default:
                break;
        }
        return std::max(radius, ActorLogic::MIN_CORNER_RADIUS);
    }

    namespace
    {
        // whether a gun's arcs meet behind it, so it turns all the way round
        bool TurnsRound(const KillstreakLogic::Gun& gun)
        {
            return gun.left + gun.right >= 360.0f;
        }

        // the clip of one of the actor's motions playing at a tick (the latest started), if any
        const Types::ActorClip* FindMotionClip(const Types::Actor& actor, std::uint32_t tick, std::string_view motion)
        {
            const Types::ActorClip* found = nullptr;
            for (const auto& clip : actor.clips)
                if (clip.animation == motion && clip.start <= tick && tick < clip.End() &&
                    (!found || clip.start >= found->start))
                    found = &clip;
            return found;
        }

        // the turns (in seconds of SPIN_RATE) a gun's barrels have spun by a time from a burst between two times
        float GetBurstSpin(float first, float last, float time)
        {
            if (time <= first)
                return 0.0f;
            // spinning up, then on at full speed, then down from as fast as it got
            const auto upTo = std::min(last, first + SPIN_UP);
            const auto rising = std::min(time, upTo) - first;
            auto spun = rising * rising / (2.0f * SPIN_UP);
            if (time <= upTo)
                return spun;
            const auto level = (upTo - first) / SPIN_UP;
            spun += std::max(std::min(time, last) - upTo, 0.0f);
            if (time <= last)
                return spun;
            const auto falling = std::min(time - last, level * SPIN_UP);
            return spun + level * falling - falling * falling / (2.0f * SPIN_UP);
        }
    }  // namespace

    Aim AimAt(const KillstreakLogic::Gun& gun, glm::vec3 direction)
    {
        Aim aim;
        if (glm::length(direction) < 1e-4f)
            return aim;
        aim.yaw = Degrees(std::atan2(direction.y, direction.x));
        if (!TurnsRound(gun))
            aim.yaw = std::clamp(aim.yaw, -gun.right, gun.left);
        aim.pitch = std::clamp(-Degrees(std::atan2(direction.z, glm::length(glm::vec2(direction)))), -gun.up, gun.down);
        return aim;
    }

    Aim GetScan(const KillstreakLogic::Gun& gun, float seconds)
    {
        if (gun.scan <= 0.0f)
            return {};
        const auto swept = std::max(seconds, 0.0f) * gun.scan;
        if (TurnsRound(gun))
            return {std::remainder(swept, 360.0f), 0.0f};
        // out to the left, across to the right, back to the middle
        const auto at = std::fmod(swept, 2.0f * (gun.left + gun.right));
        if (at < gun.left)
            return {at, 0.0f};
        if (at < 2.0f * gun.left + gun.right)
            return {gun.left - (at - gun.left), 0.0f};
        return {-gun.right + (at - 2.0f * gun.left - gun.right), 0.0f};
    }

    Aim Blend(const Aim& from, const Aim& to, float share)
    {
        return {from.yaw + std::remainder(to.yaw - from.yaw, 360.0f) * share, glm::mix(from.pitch, to.pitch, share)};
    }

    std::optional<Aim> GetIdleAim(const KillstreakLogic::Gun& gun, const Types::Actor& actor, std::uint32_t tick)
    {
        const auto scanning = FindMotionClip(actor, tick, KillstreakLogic::MOTION_SCAN);
        if (!scanning)
            return std::nullopt;
        return GetScan(gun, static_cast<float>(tick - scanning->start) / 1000.0f);
    }

    float GetSpin(std::span<const Types::ActorEvent> events, std::uint32_t tick, std::uint32_t fireTime)
    {
        auto spun = 0.0f;
        const auto seconds = [](std::uint32_t ticks) { return static_cast<float>(ticks) / 1000.0f; };
        for (const auto& event : events)
            if (event.type == Types::ActorEventType::Fire && event.tick < tick)
                spun += GetBurstSpin(seconds(event.tick), seconds(event.End() + fireTime), seconds(tick));
        return std::fmod(spun * SPIN_RATE, 360.0f);
    }

    float GetDropHeight(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto dropping = FindMotionClip(actor, tick, KillstreakLogic::MOTION_DROP);
        if (!dropping)
            return 0.0f;
        const auto seconds = static_cast<float>(tick - dropping->start) / 1000.0f;
        return std::max(KillstreakLogic::DROP_HEIGHT - 0.5f * GRAVITY * seconds * seconds, 0.0f);
    }

    std::uint32_t GetDropTicks()
    {
        return static_cast<std::uint32_t>(std::lround(std::sqrt(2.0f * KillstreakLogic::DROP_HEIGHT / GRAVITY) * 1000.0f));
    }
}  // namespace IWXMVM::Components::VehicleLogic
