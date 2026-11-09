#include "StdInclude.hpp"
#include <charconv>
#include <ranges>
#include "LightLogic.hpp"

#include "ActorLogic.hpp"
#include "KeyframeLogic.hpp"

namespace IWXMVM::Components::LightLogic
{
    namespace
    {
        constexpr std::array<std::string_view, ALL_CHANNELS.size()> CHANNEL_KEYS = {"color", "intensity", "radius",
                                                                                    "cone",  "position",  "aim"};
        constexpr std::array<std::string_view, ALL_CHANNELS.size()> CHANNEL_NAMES = {
            "Colour", "Intensity", "Radius", "Cone", "Position", "Aim"};
        // the graph's range for a position: the campaign's levels go well past the multiplayer maps'
        constexpr float MAX_POSITION = 131072.0f;

        // a spot's reach at first: it has further to go than a point (it starts where the camera is)
        constexpr float SPOT_RADIUS = 800.0f;

        std::size_t IndexOf(Channel channel)
        {
            return static_cast<std::size_t>(channel);
        }

        // the noise's value at a whole step: the same for a seed and a step, 0 to 1
        float Noise(std::uint32_t seed, std::int64_t step)
        {
            auto hash = seed * 2654435761u ^ static_cast<std::uint32_t>(step) * 2246822519u;
            hash ^= hash >> 15;
            hash *= 3266489917u;
            hash ^= hash >> 13;
            return static_cast<float>(hash % 65536u) / 65535.0f;
        }
    }  // namespace

    void Normalize(Types::ActorLight& light)
    {
        light.color = glm::clamp(light.color, 0.0f, 1.0f);
        light.intensity = std::clamp(light.intensity, 0.0f, MAX_INTENSITY);
        light.radius = std::clamp(light.radius, MIN_RADIUS, MAX_RADIUS);
        light.cone = std::clamp(light.cone, MIN_CONE, MAX_CONE);
        light.softness = std::clamp(light.softness, 0.0f, 1.0f);
        light.flickerAmount = std::clamp(light.flickerAmount, 0.0f, 1.0f);
        light.flickerRate = std::clamp(light.flickerRate, 0.0f, MAX_FLICKER_RATE);
        light.highlights = std::clamp(light.highlights, 0.0f, MAX_HIGHLIGHTS);
        light.end = std::max(light.end, light.start + MIN_BAR);
    }

    std::uint32_t MakeId(const std::vector<Types::Actor>& scene, const std::set<std::uint32_t>& keyed)
    {
        std::uint32_t id = keyed.empty() ? 0 : *keyed.rbegin();
        for (const auto& actor : scene)
            if (actor.light)
                id = std::max(id, actor.light->id);
        return id + 1;
    }

    void PlaceOnSurface(Types::Actor& actor, glm::vec3 point, glm::vec3 normal, glm::vec3 camera)
    {
        if (!actor.light)
            return;
        if (actor.light->kind == Types::LightKind::Point)
        {
            const auto out = glm::length(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0.0f, 0.0f, 1.0f);
            actor.position = point + out * POINT_OFF_SURFACE;
            return;
        }
        actor.position = camera;
        AimAt(actor, point);
        // (it reaches what was clicked, however far)
        actor.light->radius =
            std::clamp(std::max(actor.light->radius, glm::length(point - camera) * 1.25f), MIN_RADIUS, MAX_RADIUS);
    }

    void AimAt(Types::Actor& actor, glm::vec3 point)
    {
        if (!actor.light || glm::length(point - actor.position) < 0.001f)
            return;
        const auto aim = ActorLogic::GetFacing(point - actor.position);
        actor.yaw = aim.y;
        actor.light->pitch = aim.x;
    }

    Types::Actor MakeLight(Types::LightKind kind, glm::vec3 point, glm::vec3 normal, glm::vec3 camera,
                           std::uint32_t tick)
    {
        Types::Actor actor;
        actor.light = Types::ActorLight{.kind = kind, .start = tick, .end = tick + LIGHT_LENGTH};
        if (kind == Types::LightKind::Spot)
            actor.light->radius = SPOT_RADIUS;
        PlaceOnSurface(actor, point, normal, camera);
        return actor;
    }

    std::string GetBaseName(Types::LightKind kind)
    {
        return kind == Types::LightKind::Spot ? "Spot" : "Point";
    }

    std::string GetPropertyId(std::uint32_t light, Channel channel)
    {
        return std::format("light/{}/{}", light, CHANNEL_KEYS[IndexOf(channel)]);
    }

    std::string_view GetChannelName(Channel channel)
    {
        return CHANNEL_NAMES[IndexOf(channel)];
    }

    std::optional<std::uint32_t> ParseLight(std::string_view propertyId)
    {
        constexpr std::string_view PREFIX = "light/";
        if (!propertyId.starts_with(PREFIX))
            return std::nullopt;
        const auto digits = propertyId.substr(PREFIX.size());
        std::uint32_t light = 0;
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), light);
        if (error != std::errc{} || end == digits.data() || end == digits.data() + digits.size() || *end != '/')
            return std::nullopt;
        return light;
    }

    RuntimeProperties::Description Describe(std::uint32_t light, Channel channel)
    {
        RuntimeProperties::Description description;
        description.id = GetPropertyId(light, channel);
        description.name = std::format("Light {}: {}", light, CHANNEL_NAMES[IndexOf(channel)]);
        switch (channel)
        {
            case Channel::Color:
                description.valueType = Types::KeyframeValueType::Vector3;
                description.max = 1.0f;
                description.valueNames = {"R", "G", "B"};
                break;
            case Channel::Intensity:
                description.max = MAX_INTENSITY;
                break;
            case Channel::Radius:
                description.max = MAX_RADIUS;
                break;
            case Channel::Cone:
                description.max = MAX_CONE;
                break;
            case Channel::Position:
                description.valueType = Types::KeyframeValueType::Vector3;
                description.min = -MAX_POSITION;
                description.max = MAX_POSITION;
                description.valueNames = {"X", "Y", "Z"};
                break;
            case Channel::Aim:
                description.valueType = Types::KeyframeValueType::Vector3;
                description.min = -360.0f;
                description.max = 360.0f;
                description.valueNames = {"Pitch", "Yaw", ""};
                break;
        }
        return description;
    }

    Types::KeyframeValue GetValue(const Types::ActorLight& light, Channel channel)
    {
        switch (channel)
        {
            case Channel::Color:
                return light.color;
            case Channel::Intensity:
                return light.intensity;
            case Channel::Radius:
                return light.radius;
            case Channel::Cone:
                return light.cone;
            default:
                break;
        }
        return 0.0f;
    }

    void SetValue(Types::ActorLight& light, Channel channel, const Types::KeyframeValue& value)
    {
        switch (channel)
        {
            case Channel::Color:
                light.color = value.vector3;
                break;
            case Channel::Intensity:
                light.intensity = value.floatingPoint;
                break;
            case Channel::Radius:
                light.radius = value.floatingPoint;
                break;
            case Channel::Cone:
                light.cone = value.floatingPoint;
                break;
            default:
                break;
        }
    }

    Types::KeyframeValue GetValue(const Types::Actor& actor, Channel channel)
    {
        if (!actor.light)
            return 0.0f;
        if (channel == Channel::Position)
            return actor.light->follow ? actor.light->follow->offset : actor.position;
        if (channel == Channel::Aim)
            return glm::vec3(actor.light->pitch, actor.yaw, 0.0f);
        return GetValue(*actor.light, channel);
    }

    void SetValue(Types::Actor& actor, Channel channel, const Types::KeyframeValue& value)
    {
        if (!actor.light)
            return;
        if (channel == Channel::Position)
        {
            (actor.light->follow ? actor.light->follow->offset : actor.position) = value.vector3;
        }
        else if (channel == Channel::Aim)
        {
            actor.light->pitch = value.vector3.x;
            actor.yaw = value.vector3.y;
        }
        else
        {
            SetValue(*actor.light, channel, value);
        }
    }

    Types::Actor ApplyPlacementKeys(Types::Actor actor, const Keys& keys, double tick)
    {
        if (const auto& position = keys[IndexOf(Channel::Position)]; !position.empty())
            SetValue(actor, Channel::Position,
                     KeyframeLogic::Evaluate(Types::KeyframeValueType::Vector3, position, tick));
        if (const auto& aim = keys[IndexOf(Channel::Aim)]; !aim.empty())
            SetValue(actor, Channel::Aim,
                     KeyframeLogic::Evaluate(Types::KeyframeValueType::Vector3, UnwrapAim(aim), tick));
        return actor;
    }

    float NearestYaw(float yaw, float to)
    {
        return to + std::remainder(yaw - to, 360.0f);
    }

    std::vector<Types::Keyframe> UnwrapAim(std::vector<Types::Keyframe> keys)
    {
        for (std::size_t i = 1; i < keys.size(); ++i)
            keys[i].value.vector3.y = NearestYaw(keys[i].value.vector3.y, keys[i - 1].value.vector3.y);
        return keys;
    }

    glm::vec3 GetAimForKey(const std::vector<Types::Keyframe>& keys, std::uint32_t tick, glm::vec3 aim)
    {
        const auto unwrapped = UnwrapAim(keys);
        const auto before = std::ranges::find_if(unwrapped | std::views::reverse,
                                                 [&](const Types::Keyframe& key) { return key.tick < tick; });
        const auto after = std::ranges::find_if(unwrapped, [&](const Types::Keyframe& key) { return key.tick > tick; });
        if (before != std::ranges::end(unwrapped | std::views::reverse))
            aim.y = NearestYaw(aim.y, before->value.vector3.y);
        else if (after != unwrapped.end())
            aim.y = NearestYaw(aim.y, after->value.vector3.y);
        else
            aim.y = std::remainder(aim.y, 360.0f);
        return aim;
    }

    std::vector<glm::vec3> GetPathLine(const std::vector<Types::Keyframe>& positionKeys)
    {
        constexpr float SAMPLES_PER_UNIT = 0.05f;
        // (an eased key bends between its keys: a few points even for keys close together)
        constexpr float LEAST_SAMPLES = 8.0f;
        constexpr float MOST_SAMPLES = 400.0f;
        std::vector<glm::vec3> line;
        if (positionKeys.size() < 2)
            return line;
        for (std::size_t i = 0; i + 1 < positionKeys.size(); ++i)
        {
            const auto& from = positionKeys[i];
            const auto& to = positionKeys[i + 1];
            const auto distance = glm::distance(from.value.vector3, to.value.vector3);
            const auto samples = std::clamp(distance * SAMPLES_PER_UNIT, LEAST_SAMPLES, MOST_SAMPLES);
            for (float step = 0.0f; step < samples; step += 1.0f)
            {
                const auto tick = from.tick + (static_cast<double>(to.tick) - from.tick) * step / samples;
                line.push_back(KeyframeLogic::Evaluate(Types::KeyframeValueType::Vector3, positionKeys, tick).vector3);
            }
        }
        line.push_back(positionKeys.back().value.vector3);
        return line;
    }

    std::optional<std::string> GetViewGunBone(Types::LightFirstPerson firstPerson, std::string_view bone,
                                              std::span<const std::string> armBones,
                                              std::span<const std::string> gunBones)
    {
        if (firstPerson == Types::LightFirstPerson::Body)
            return std::nullopt;
        // (the gun's root sits where the body's hand holds it)
        const std::string name = bone == "tag_weapon_right" ? "j_gun" : std::string(bone);
        const bool onGun = std::ranges::find(gunBones, name) != gunBones.end();
        const bool onArms = std::ranges::find(armBones, name) != armBones.end();
        if (onGun || (firstPerson == Types::LightFirstPerson::ViewGun && onArms))
            return name;
        return std::nullopt;
    }

    Types::ActorLight ApplyKeys(Types::ActorLight light, const Keys& keys, double tick)
    {
        for (const auto channel : CHANNELS)
        {
            const auto& channelKeys = keys[IndexOf(channel)];
            if (channelKeys.empty())
                continue;
            const auto type =
                channel == Channel::Color ? Types::KeyframeValueType::Vector3 : Types::KeyframeValueType::FloatingPoint;
            SetValue(light, channel, KeyframeLogic::Evaluate(type, channelKeys, tick));
        }
        Normalize(light);
        return light;
    }

    float GetFlicker(std::uint32_t seed, float amount, float rate, double tick)
    {
        if (amount <= 0.0f || rate <= 0.0f)
            return 1.0f;
        const auto position = tick / 1000.0 * rate;
        const auto step = std::floor(position);
        const auto from = Noise(seed, static_cast<std::int64_t>(step));
        const auto to = Noise(seed, static_cast<std::int64_t>(step) + 1);
        const auto along = static_cast<float>(position - step);
        const auto eased = along * along * (3.0f - 2.0f * along);
        return 1.0f - amount * (from + (to - from) * eased);
    }

    float ToLinear(float gamma)
    {
        return gamma > 0.04045f ? std::pow((gamma + 0.055f) / 1.055f, 2.4f) : gamma / 12.92f;
    }

    glm::vec3 ToLinear(glm::vec3 color)
    {
        return {ToLinear(color.r), ToLinear(color.g), ToLinear(color.b)};
    }

    std::optional<State> GetState(const Types::Actor& actor, const Keys& keys, double tick)
    {
        return GetState(actor, keys, tick, tick);
    }

    std::optional<State> GetState(const Types::Actor& actor, const Keys& keys, double tick, double keysTick)
    {
        if (!actor.light || actor.hidden || tick < actor.light->start || tick >= actor.light->end)
            return std::nullopt;
        const auto light = ApplyKeys(*actor.light, keys, keysTick);
        const auto intensity =
            light.intensity * GetFlicker(actor.light->id, light.flickerAmount, light.flickerRate, tick);
        if (intensity <= 0.0f)
            return std::nullopt;
        return State{.kind = light.kind,
                     .color = ToLinear(light.color) * intensity,
                     .radius = light.radius,
                     .cone = light.cone,
                     .softness = light.softness,
                     .shadows = light.kind == Types::LightKind::Spot && light.shadows,
                     .highlights = light.highlights};
    }

    glm::vec3 GetAim(float pitch, float yaw)
    {
        const auto pitchRadians = glm::radians(pitch);
        const auto yawRadians = glm::radians(yaw);
        return {std::cos(pitchRadians) * std::cos(yawRadians), std::cos(pitchRadians) * std::sin(yawRadians),
                -std::sin(pitchRadians)};
    }

    Placed Place(const Types::Actor& actor)
    {
        return {actor.position, actor.light ? GetAim(actor.light->pitch, actor.yaw) : glm::vec3(1.0f, 0.0f, 0.0f)};
    }

    Placed Place(const Types::Actor& actor, glm::vec3 bonePosition, const glm::mat3& boneAxes)
    {
        if (!actor.light || !actor.light->follow)
            return Place(actor);
        return {bonePosition + boneAxes * actor.light->follow->offset,
                boneAxes * GetAim(actor.light->pitch, actor.yaw)};
    }

    ScreenRect GetScissor(glm::vec3 origin, float radius, const glm::mat4& viewProjection, const ScreenRect& viewport)
    {
        // (in front of the near plane by a little: a corner almost at the camera's plane projects far off)
        constexpr float NEAREST = 0.01f;
        auto least = glm::vec2(1.0f), most = glm::vec2(-1.0f);
        for (const auto x : {-radius, radius})
            for (const auto y : {-radius, radius})
                for (const auto z : {-radius, radius})
                {
                    const auto corner = viewProjection * glm::vec4(origin + glm::vec3(x, y, z), 1.0f);
                    if (corner.w < NEAREST)
                        return viewport;
                    const auto projected = glm::vec2(corner.x, corner.y) / corner.w;
                    least = glm::min(least, projected);
                    most = glm::max(most, projected);
                }
        least = glm::clamp(least, -1.0f, 1.0f);
        most = glm::clamp(most, -1.0f, 1.0f);
        if (least.x >= most.x || least.y >= most.y)
            return {viewport.x, viewport.y, 0, 0};

        // (y goes down in pixels and up in the view)
        const auto left = static_cast<std::int32_t>(std::floor((least.x + 1.0f) * 0.5f * viewport.width));
        const auto right = static_cast<std::int32_t>(std::ceil((most.x + 1.0f) * 0.5f * viewport.width));
        const auto top = static_cast<std::int32_t>(std::floor((1.0f - most.y) * 0.5f * viewport.height));
        const auto bottom = static_cast<std::int32_t>(std::ceil((1.0f - least.y) * 0.5f * viewport.height));
        return {viewport.x + left, viewport.y + top, right - left, bottom - top};
    }

    std::optional<std::uint32_t> GetShadowed(const std::vector<Types::Actor>& scene,
                                             const std::vector<std::optional<State>>& states)
    {
        for (std::size_t i = 0; i < std::min(scene.size(), states.size()); ++i)
            if (states[i] && states[i]->shadows)
                return scene[i].id;
        return std::nullopt;
    }

    bool LosesShadows(const std::vector<Types::Actor>& scene, std::uint32_t actor, double tick)
    {
        std::vector<std::optional<State>> states;
        for (const auto& each : scene)
            states.push_back(GetState(each, {}, tick));
        const auto it = std::ranges::find(scene, actor, &Types::Actor::id);
        if (it == scene.end() || !states[it - scene.begin()] || !states[it - scene.begin()]->shadows)
            return false;
        return GetShadowed(scene, states) != actor;
    }

    std::vector<std::vector<glm::vec3>> GetIconLines(glm::vec3 position, glm::vec3 right, glm::vec3 up, float size)
    {
        constexpr int RAYS = 8;
        constexpr int RING = 16;
        std::vector<std::vector<glm::vec3>> lines;
        const auto at = [&](float angle, float reach) {
            return position + (right * std::cos(angle) + up * std::sin(angle)) * reach;
        };
        std::vector<glm::vec3> ring;
        for (int i = 0; i <= RING; ++i)
            ring.push_back(at(glm::two_pi<float>() * i / RING, size * 0.5f));
        lines.push_back(std::move(ring));
        for (int i = 0; i < RAYS; ++i)
        {
            const auto angle = glm::two_pi<float>() * i / RAYS;
            lines.push_back({at(angle, size * 0.75f), at(angle, size * 1.2f)});
        }
        return lines;
    }

    std::vector<std::vector<glm::vec3>> GetReachLines(Types::LightKind kind, const Placed& placed, float radius,
                                                      float cone)
    {
        constexpr int SEGMENTS = 48;
        const auto circle = [&](glm::vec3 center, glm::vec3 a, glm::vec3 b, float size) {
            std::vector<glm::vec3> points;
            for (int i = 0; i <= SEGMENTS; ++i)
            {
                const auto angle = glm::two_pi<float>() * i / SEGMENTS;
                points.push_back(center + (a * std::cos(angle) + b * std::sin(angle)) * size);
            }
            return points;
        };
        if (kind == Types::LightKind::Point)
        {
            const glm::vec3 x(1.0f, 0.0f, 0.0f), y(0.0f, 1.0f, 0.0f), z(0.0f, 0.0f, 1.0f);
            return {circle(placed.position, x, y, radius), circle(placed.position, x, z, radius),
                    circle(placed.position, y, z, radius)};
        }
        const auto forward = glm::normalize(placed.direction);
        auto side = glm::cross(forward, glm::vec3(0.0f, 0.0f, 1.0f));
        if (glm::length(side) < 0.001f)
            side = glm::vec3(0.0f, 1.0f, 0.0f);
        side = glm::normalize(side);
        const auto top = glm::cross(side, forward);
        const auto half = glm::radians(cone / 2.0f);
        const auto center = placed.position + forward * radius * std::cos(half);
        const auto rim = radius * std::sin(half);
        std::vector<std::vector<glm::vec3>> lines{circle(center, side, top, rim)};
        for (const auto& edge : {side, -side, top, -top})
            lines.push_back({placed.position, center + edge * rim});
        return lines;
    }
}  // namespace IWXMVM::Components::LightLogic
