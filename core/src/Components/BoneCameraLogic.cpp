#include "StdInclude.hpp"
#include "BoneCameraLogic.hpp"

#include "KeyframeLogic.hpp"

namespace IWXMVM::Components::BoneCameraLogic
{
    glm::mat3 AnglesToAxes(const glm::vec3& angles)
    {
        const auto sp = std::sin(glm::radians(angles[0])), cp = std::cos(glm::radians(angles[0]));
        const auto sy = std::sin(glm::radians(angles[1])), cy = std::cos(glm::radians(angles[1]));
        const auto sr = std::sin(glm::radians(angles[2])), cr = std::cos(glm::radians(angles[2]));
        const glm::vec3 forward(cp * cy, cp * sy, -sp);
        const glm::vec3 left(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp);
        const glm::vec3 up(cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp);
        return glm::mat3(forward, left, up);
    }

    glm::vec3 AxesToAngles(const glm::mat3& axes)
    {
        const auto& forward = axes[0];
        const auto& left = axes[1];
        const auto& up = axes[2];
        const auto pitch = -glm::degrees(std::asin(std::clamp(forward.z, -1.0f, 1.0f)));
        // straight up or down: the roll can't be told from the yaw, so it is taken as none
        if (std::abs(forward.z) > 0.99999f)
            return glm::vec3(pitch, glm::degrees(std::atan2(-left.x, left.y)), 0.0f);
        return glm::vec3(pitch, glm::degrees(std::atan2(forward.y, forward.x)),
                         glm::degrees(std::atan2(left.z, up.z)));
    }

    Transform MakeFrame(const glm::vec3& origin, float yaw)
    {
        return {origin, AnglesToAxes(glm::vec3(0.0f, yaw, 0.0f))};
    }

    namespace
    {
        // the axes turned about the world's up only, so that they look the same way with no roll
        glm::mat3 Level(const glm::mat3& axes)
        {
            const auto forward = glm::normalize(axes[0]);
            const auto left = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), forward);
            if (glm::length(left) < 0.0001f)
                return axes;
            const auto leftUnit = glm::normalize(left);
            return glm::mat3(forward, leftUnit, glm::cross(forward, leftUnit));
        }

        // the axes the position offset is in, and those the rotation offset turns from
        const glm::mat3& GetOffsetAxes(const Transform& bone, const Transform& frame,
                                       const Types::BoneCameraSettings& settings)
        {
            return settings.turn == Types::BoneCameraTurn::Facing ? frame.axes : bone.axes;
        }

        std::string ToLower(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }
    }  // namespace

    Placement Place(const Transform& bone, const Transform& frame, const Types::BoneCameraSettings& settings)
    {
        const auto& offsetAxes = GetOffsetAxes(bone, frame, settings);
        auto axes = offsetAxes * AnglesToAxes(settings.rotationOffset);
        if (settings.turn == Types::BoneCameraTurn::Level)
            axes = Level(axes);
        return {bone.position + offsetAxes * settings.positionOffset, AxesToAngles(axes)};
    }

    std::pair<glm::vec3, glm::vec3> GetOffsets(const Transform& bone, const Transform& frame,
                                               const Types::BoneCameraSettings& settings, const glm::vec3& position,
                                               const glm::vec3& angles)
    {
        const auto toOffset = glm::transpose(GetOffsetAxes(bone, frame, settings));
        return {toOffset * (position - bone.position), AxesToAngles(toOffset * AnglesToAxes(angles))};
    }

    Transform Steady::Step(const Transform& bone, const Transform& frame, std::uint32_t now, float seconds,
                           const Key& stepKey)
    {
        const auto toFrame = glm::transpose(frame.axes);
        const auto sampled = toFrame * (bone.position - frame.position);
        auto sampledRotation = glm::quat_cast(toFrame * bone.axes);

        const bool jump = !key || *key != stepKey || now < tick || now - tick > MAX_STEADY_STEP || seconds <= 0.0f;
        if (jump)
        {
            position = sampled;
            rotation = sampledRotation;
        }
        else if (now != tick)
        {
            const auto t = 1.0f - std::exp(-static_cast<float>(now - tick) / 1000.0f / seconds);
            position = glm::mix(position, sampled, t);
            if (glm::dot(rotation, sampledRotation) < 0.0f)
                sampledRotation = -sampledRotation;
            rotation = glm::normalize(glm::slerp(rotation, sampledRotation, t));
        }
        key = stepKey;
        tick = now;
        return {frame.position + frame.axes * position, frame.axes * glm::mat3_cast(rotation)};
    }

    void Steady::Reset()
    {
        key.reset();
    }

    namespace
    {
        // Glide's: it starts when a frame takes less than this much of a millisecond of the film, and ends at the second
        // (the two apart, as a frame's length wanders about them at 0.1 and 100 fps)
        constexpr double GLIDE_STARTS = 0.8;
        constexpr double GLIDE_ENDS = 0.95;
        // how far a frame's length moves its average
        constexpr double FRAME_AVERAGING = 0.1;
        // a view further off than this is a cut (someone spawned): a bone does about 0.5 units in a millisecond
        constexpr float MAX_GLIDE_DISTANCE = 32.0f;
    }  // namespace

    View Glide::Step(const View& exact, std::uint32_t now, double time, float speed, const Steady::Key& stepKey)
    {
        const auto rotation = glm::quat_cast(AnglesToAxes(exact.angles));
        const bool cut =
            !samples.empty() && glm::distance(samples.back().position, exact.position) > MAX_GLIDE_DISTANCE;
        const bool jump = !key || *key != stepKey || now < tick || now - tick > MAX_STEADY_STEP || speed <= 0.0f || cut;
        if (key)
        {
            const auto frame = time - lastFrame;
            frameLength = frameLength > 0.0 ? frameLength + (frame - frameLength) * FRAME_AVERAGING : frame;
        }
        gliding = speed * frameLength < (gliding ? GLIDE_ENDS : GLIDE_STARTS);
        if (jump || now != tick)
            tickTime = time;
        key = stepKey;
        tick = now;
        lastFrame = time;

        // a millisecond of the clock in real time; once it has stood that long, nothing more is on its way
        const auto window = speed > 0.0f ? 1.0 / speed : 0.0;
        if (jump || !gliding || time - tickTime > window)
        {
            samples.assign(1, {time, exact.position, rotation, exact.fov});
            return exact;
        }

        samples.push_back({time, exact.position, rotation, exact.fov});
        const auto start = time - window;
        while (samples.front().time <= start)
            samples.pop_front();

        auto position = glm::dvec3(0.0);
        auto turned = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
        double fov = 0.0;
        auto from = start;
        for (const auto& sample : samples)
        {
            const auto weight = sample.time - from;
            from = sample.time;
            position += glm::dvec3(sample.position) * weight;
            fov += sample.fov * weight;
            const auto share = static_cast<float>(weight / window);
            turned += (glm::dot(sample.rotation, rotation) < 0.0f ? -sample.rotation : sample.rotation) * share;
        }
        return {glm::vec3(position / window), AxesToAngles(glm::mat3_cast(glm::normalize(turned))),
                static_cast<float>(fov / window)};
    }

    void Glide::Reset()
    {
        key.reset();
        frameLength = 0.0;
        gliding = false;
        samples.clear();
    }

    std::span<const NamedBone> GetNamedBones()
    {
        static constexpr std::array<NamedBone, 12> BONES = {{
            {"tag_eye", "Eyes"},
            {"j_head", "Head"},
            {"j_neck", "Neck"},
            {"j_spine4", "Chest"},
            {"pelvis", "Pelvis"},
            {"tag_origin", "Origin"},
            {"j_wrist_ri", "Right hand"},
            {"j_wrist_le", "Left hand"},
            {"tag_weapon_right", "Gun"},
            {"tag_flash", "Muzzle"},
            {"j_ankle_ri", "Right foot"},
            {"j_ankle_le", "Left foot"},
        }};
        return BONES;
    }

    std::string DescribeBone(std::string_view bone)
    {
        const auto named = GetNamedBones();
        const auto it = std::ranges::find(named, bone, &NamedBone::bone);
        return std::string(it != named.end() ? it->label : bone);
    }

    std::pair<std::vector<std::string>, std::size_t> OrderBones(std::span<const std::string> bones)
    {
        std::vector<std::string> ordered;
        for (const auto& named : GetNamedBones())
        {
            if (std::ranges::find(bones, named.bone) != bones.end())
                ordered.emplace_back(named.bone);
        }
        const auto namedCount = ordered.size();
        for (const auto& bone : bones)
        {
            if (std::ranges::find(ordered, bone) == ordered.end())
                ordered.push_back(bone);
        }
        return {ordered, namedCount};
    }

    std::string DescribeChange(const Types::BoneCameraSettings& before, const Types::BoneCameraSettings& after,
                               const std::string& targetName)
    {
        const auto boneName = ToLower(DescribeBone(after.bone));
        if (before.target != after.target)
            return before.bone != after.bone ? std::format("follow {}'s {}", targetName, boneName)
                                             : std::format("follow {}", targetName);

        const std::array changed = {before.bone != after.bone,
                                    before.positionOffset != after.positionOffset,
                                    before.rotationOffset != after.rotationOffset,
                                    before.fov != after.fov,
                                    before.steady != after.steady,
                                    before.turn != after.turn,
                                    before.showBone != after.showBone};
        if (std::ranges::count(changed, true) > 1)
            return "change the bone camera";
        if (changed[0])
            return std::format("put the bone camera on the {}", boneName);
        if (changed[1])
            return "move the bone camera";
        if (changed[2])
            return "turn the bone camera";
        if (changed[3])
            return "change the bone camera's FOV";
        if (changed[4])
            return "change Steady";
        if (changed[5])
            return "change how the bone camera turns";
        if (changed[6])
            return after.showBone ? "show the bone" : "hide the bone";
        return "change the bone camera";
    }

    bool IsKeyedProperty(Types::KeyframeablePropertyType type)
    {
        return std::ranges::find(KEYED_PROPERTIES, type) != KEYED_PROPERTIES.end();
    }

    Keyed GetKeyed(const Keys& keys)
    {
        return {!keys.offset.empty(), !keys.rotation.empty(), !keys.fov.empty()};
    }

    Types::BoneCameraSettings ApplyKeys(Types::BoneCameraSettings settings, const Keys& keys, double tick)
    {
        // (the values as the numbers go, as the graph draws them: a rotation's keys aren't unwrapped)
        const auto at = [tick](Types::KeyframeValueType type, const std::vector<Types::Keyframe>& each) {
            return KeyframeLogic::Evaluate(type, each, tick);
        };
        if (!keys.offset.empty())
            settings.positionOffset = at(Types::KeyframeValueType::Vector3, keys.offset).vector3;
        if (!keys.rotation.empty())
            settings.rotationOffset = at(Types::KeyframeValueType::Vector3, keys.rotation).vector3;
        if (!keys.fov.empty())
            settings.fov = at(Types::KeyframeValueType::FloatingPoint, keys.fov).floatingPoint;
        return settings;
    }

    Types::BoneCameraSettings WithKeyed(Types::BoneCameraSettings settings, const Types::BoneCameraSettings& from,
                                        Keyed keyed)
    {
        if (keyed.offset)
            settings.positionOffset = from.positionOffset;
        if (keyed.rotation)
            settings.rotationOffset = from.rotationOffset;
        if (keyed.fov)
            settings.fov = from.fov;
        return settings;
    }

    std::optional<Types::KeyframeValue> GetValue(const Types::BoneCameraSettings& settings,
                                                 Types::KeyframeablePropertyType type)
    {
        switch (type)
        {
            case Types::KeyframeablePropertyType::BoneCameraOffset:
                return Types::KeyframeValue(settings.positionOffset);
            case Types::KeyframeablePropertyType::BoneCameraRotation:
                return Types::KeyframeValue(settings.rotationOffset);
            case Types::KeyframeablePropertyType::BoneCameraFov:
                return Types::KeyframeValue(settings.fov);
            default:
                return std::nullopt;
        }
    }

    bool SetValue(Types::BoneCameraSettings& settings, Types::KeyframeablePropertyType type,
                  const Types::KeyframeValue& value)
    {
        switch (type)
        {
            case Types::KeyframeablePropertyType::BoneCameraOffset:
                settings.positionOffset = value.vector3;
                return true;
            case Types::KeyframeablePropertyType::BoneCameraRotation:
                settings.rotationOffset = value.vector3;
                return true;
            case Types::KeyframeablePropertyType::BoneCameraFov:
                settings.fov = value.floatingPoint;
                return true;
            default:
                return false;
        }
    }

    bool Edit::Track(const Types::BoneCameraSettings& frameStart, const Types::BoneCameraSettings& now, Keyed keyed)
    {
        if (before || WithKeyed(frameStart, now, keyed) == now)
            return false;
        before = frameStart;
        return true;
    }

    std::optional<std::pair<Types::BoneCameraSettings, Types::BoneCameraSettings>> Edit::End(
        const Types::BoneCameraSettings& now, Keyed keyed)
    {
        if (!before)
            return std::nullopt;
        const auto was = WithKeyed(*before, now, keyed);
        before.reset();
        if (was == now)
            return std::nullopt;
        return std::pair(was, now);
    }
}  // namespace IWXMVM::Components::BoneCameraLogic
