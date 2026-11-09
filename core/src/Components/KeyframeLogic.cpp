#include "StdInclude.hpp"
#include "KeyframeLogic.hpp"

#include "RuntimeProperties.hpp"

#include "glm/gtx/euler_angles.hpp"

namespace IWXMVM::KeyframeLogic
{
    bool IsAngle(Types::KeyframeValueType type, std::uint32_t index)
    {
        return type == Types::KeyframeValueType::CameraData && index >= 3 && index <= 5;
    }

    void Unwrap(std::span<float> degrees)
    {
        for (std::size_t i = 1; i < degrees.size(); ++i)
        {
            const auto turns = std::round((degrees[i] - degrees[i - 1]) / 360.0f);
            degrees[i] -= turns * 360.0f;
        }
    }

    std::vector<float> GetValues(std::span<const Types::Keyframe> keys, std::uint32_t index, bool angle)
    {
        std::vector<float> values;
        values.reserve(keys.size());
        for (const auto& key : keys)
            values.push_back(key.value.GetByIndex(index));
        if (angle)
            Unwrap(values);
        return values;
    }

    double Ease(double fraction, float easeOut, float easeIn)
    {
        // a cubic from 0 to 1 whose slopes at its ends are 1 less the eases: 1 and 1 is the fraction itself
        const double t = std::clamp(fraction, 0.0, 1.0);
        const double leaving = 1.0 - std::clamp(easeOut, 0.0f, 1.0f);
        const double arriving = 1.0 - std::clamp(easeIn, 0.0f, 1.0f);
        const double t2 = t * t, t3 = t2 * t;
        return (t3 - 2 * t2 + t) * leaving + (-2 * t3 + 3 * t2) + (t3 - t2) * arriving;
    }

    double Uneased(double eased, float easeOut, float easeIn)
    {
        double low = 0.0, high = 1.0;
        for (int i = 0; i < 50; ++i)
        {
            const double middle = (low + high) / 2;
            (Ease(middle, easeOut, easeIn) < eased ? low : high) = middle;
        }
        return (low + high) / 2;
    }

    double GetSmoothSlope(std::span<const Types::Keyframe> keys, std::span<const float> values, std::size_t key)
    {
        if (key == 0 || key + 1 >= keys.size())
            return 0.0;
        const double before = static_cast<double>(keys[key].tick) - keys[key - 1].tick;
        const double after = static_cast<double>(keys[key + 1].tick) - keys[key].tick;
        if (before <= 0.0 || after <= 0.0)
            return 0.0;
        const double slopeBefore = (values[key] - values[key - 1]) / before;
        const double slopeAfter = (values[key + 1] - values[key]) / after;
        // (where the values turn, or stay)
        if (slopeBefore * slopeAfter <= 0.0)
            return 0.0;
        const double slope = (slopeBefore * after + slopeAfter * before) / (before + after);
        const double limit = 3.0 * std::min(std::abs(slopeBefore), std::abs(slopeAfter));
        return std::clamp(slope, -limit, limit);
    }

    float InterpolateLinear(std::span<const float> ticks, std::span<const float> values, float tick)
    {
        if (values.size() == 1)
            return values.front();

        std::size_t p0 = 0, p1 = 1;
        for (std::size_t i = 0; i < values.size() - 1; i++)
        {
            if (tick >= ticks[i] && tick <= ticks[i + 1])
            {
                p0 = i;
                p1 = i + 1;
                break;
            }
        }
        const float t = (tick - ticks[p0]) / (ticks[p1] - ticks[p0]);
        return (1.0f - t) * values[p0] + t * values[p1];
    }

    // Copyright (c) by NUMERICAL RECIPES IN C: THE ART OF SCIENTIFIC COMPUTING (ISBN 0-521-43108-5)
    // Modified. Thank you to dtugend for finding this! (Moved here from MathUtils as it was, for "Auto".)
    float InterpolateCubicSpline(std::span<const float> ticks, std::span<const float> values, float tick)
    {
        const size_t n = values.size();
        if (n < 2)
            return n == 1 ? values.front() : 0.0f;

        constexpr int32_t MAX_NODES = 256;
        if (n > MAX_NODES)
            return values.back();

        float y2[MAX_NODES];  // second derivatives
        float u[MAX_NODES];

        y2[0] = -0.5f;
        u[0] = (3.0f / (ticks[1] - ticks[0])) * ((values[1] - values[0]) / (ticks[1] - ticks[0]));

        for (size_t i = 1; i <= n - 2; i++)
        {
            const auto prevTick = ticks[i - 1];
            const auto prevValue = values[i - 1];
            const auto currTick = ticks[i];
            const auto currValue = values[i];
            const auto nextTick = ticks[i + 1];
            const auto nextValue = values[i + 1];

            auto sig = (currTick - prevTick) / (nextTick - prevTick);
            auto p = sig * y2[i - 1] + 2.0f;
            y2[i] = (sig - 1.0f) / p;
            u[i] = (nextValue - currValue) / (nextTick - currTick) - (currValue - prevValue) / (currTick - prevTick);
            u[i] = (6.0f * u[i] / (nextTick - prevTick) - sig * u[i - 1]) / p;
        }

        auto qn = 0.5f;
        auto un = (3.0f / (ticks[n - 1] - ticks[n - 2])) *
                  (0.0f - (values[n - 1] - values[n - 2]) / (ticks[n - 1] - ticks[n - 2]));

        y2[n - 1] = (un - qn * u[n - 2]) / (qn * y2[n - 2] + 1.0f);

        for (int k = static_cast<int>(n) - 2; k >= 0; k--)
            y2[k] = y2[k] * y2[k + 1] + u[k];

        int klo = 0;
        int khi = static_cast<int>(n) - 1;
        while (khi - klo > 1)
        {
            int k = (khi + klo) >> 1;
            if (ticks[k] > tick)
                khi = k;
            else
                klo = k;
        }
        auto h = ticks[khi] - ticks[klo];
        auto a = (ticks[khi] - tick) / h;
        auto b = (tick - ticks[klo]) / h;
        return a * values[klo] + b * values[khi] +
               ((a * a * a - a) * y2[klo] + (b * b * b - b) * y2[khi]) * (h * h) / 6.0f;
    }

    float Evaluate(std::span<const Types::Keyframe> keys, std::uint32_t index, bool angle, double tick, bool eased)
    {
        if (keys.empty())
            return 0.0f;
        return Evaluate(keys, GetValues(keys, index, angle), tick, eased);
    }

    float Evaluate(std::span<const Types::Keyframe> keys, std::span<const float> values, double tick, bool eased)
    {
        if (keys.empty() || values.size() != keys.size())
            return 0.0f;
        if (tick <= keys.front().tick)
            return values.front();
        if (tick >= keys.back().tick)
            return values.back();

        const auto next = std::upper_bound(keys.begin(), keys.end(), tick,
                                           [](double at, const Types::Keyframe& key) { return at < key.tick; });
        const auto i = static_cast<std::size_t>(next - keys.begin()) - 1;
        const auto& key = keys[i];
        const auto& after = keys[i + 1];
        if (key.interpolation == Types::KeyInterpolation::Hold)
            return values[i];

        const double span = static_cast<double>(after.tick) - key.tick;
        const bool easing = eased && (key.easeOut > 0.0f || after.easeIn > 0.0f);
        double fraction = (tick - key.tick) / span;
        if (easing)
            fraction = Ease(fraction, key.easeOut, after.easeIn);

        switch (key.interpolation)
        {
            case Types::KeyInterpolation::Auto:
            {
                // (in floats, as it was, so older files look exactly as they did)
                std::vector<float> ticks;
                ticks.reserve(keys.size());
                for (const auto& each : keys)
                    ticks.push_back(static_cast<float>(each.tick));
                const auto at = easing ? static_cast<float>(key.tick + fraction * span) : static_cast<float>(tick);
                return keys.size() < 4 ? InterpolateLinear(ticks, values, at)
                                       : InterpolateCubicSpline(ticks, values, at);
            }
            case Types::KeyInterpolation::Linear:
                return static_cast<float>(values[i] + (values[i + 1] - values[i]) * fraction);
            default:
            {
                // a cubic Hermite curve with the slopes Smooth gives the two keys
                const double t = fraction, t2 = t * t, t3 = t2 * t;
                const double leaving = GetSmoothSlope(keys, values, i) * span;
                const double arriving = GetSmoothSlope(keys, values, i + 1) * span;
                return static_cast<float>((2 * t3 - 3 * t2 + 1) * values[i] + (t3 - 2 * t2 + t) * leaving +
                                          (-2 * t3 + 3 * t2) * values[i + 1] + (t3 - t2) * arriving);
            }
        }
    }

    Types::KeyframeValue Evaluate(Types::KeyframeValueType type, std::span<const Types::Keyframe> keys, double tick,
                                  bool eased)
    {
        const auto value = [&](std::uint32_t index) {
            return Evaluate(keys, index, IsAngle(type, index), tick, eased);
        };
        switch (type)
        {
            case Types::KeyframeValueType::FloatingPoint:
                return Types::KeyframeValue(value(0));
            case Types::KeyframeValueType::Vector3:
                return Types::KeyframeValue(glm::vec3(value(0), value(1), value(2)));
            case Types::KeyframeValueType::CameraData:
                return Types::KeyframeValue(Types::CameraData{glm::vec3(value(0), value(1), value(2)),
                                                              glm::vec3(value(3), value(4), value(5)), value(6)});
        }
        return Types::KeyframeValue(0.0f);
    }

    Types::KeyInterpolation GetDefaultInterpolation(Types::KeyframeablePropertyType type)
    {
        if (IsStepped(type))
            return Types::KeyInterpolation::Hold;
        return type == Types::KeyframeablePropertyType::CampathCamera ? Types::KeyInterpolation::Auto
                                                                      : Types::KeyInterpolation::Smooth;
    }

    Types::KeyInterpolation GetNewKeyInterpolation(std::span<const Types::Keyframe> keys, std::uint32_t tick,
                                                   Types::KeyframeablePropertyType type)
    {
        if (keys.empty() || IsStepped(type))
            return GetDefaultInterpolation(type);
        const Types::Keyframe* before = nullptr;
        for (const auto& key : keys)
        {
            if (key.tick <= tick)
                before = &key;
        }
        return (before ? *before : keys.front()).interpolation;
    }

    bool IsStepped(Types::KeyframeablePropertyType type)
    {
        using Type = Types::KeyframeablePropertyType;
        if (const auto runtime = Components::RuntimeProperties::Find(type))
            return runtime->steps != Components::RuntimeProperties::Steps::None;
        return type == Type::FilmtweakEnabled || type == Type::FilmtweakInvert || type == Type::DepthOfFieldEnabled;
    }

    float GetStepValue(float value)
    {
        return value >= 0.5f ? 1.0f : 0.0f;
    }

    float GetStepValue(Types::KeyframeablePropertyType type, float value)
    {
        if (const auto runtime = Components::RuntimeProperties::Find(type);
            runtime && runtime->steps == Components::RuntimeProperties::Steps::Whole)
            return std::round(value);
        return GetStepValue(value);
    }

    Types::Keyframe Normalize(Types::Keyframe key, Types::KeyframeablePropertyType type)
    {
        if (!IsStepped(type))
            return key;
        key.interpolation = Types::KeyInterpolation::Hold;
        key.easeIn = 0.0f;
        key.easeOut = 0.0f;
        key.value.floatingPoint = GetStepValue(type, key.value.floatingPoint);
        return key;
    }

    bool ApplyAutoEase(std::span<Types::Keyframe> nodes, float start, float end, Types::AutoEaseNodes which)
    {
        const bool all = which == Types::AutoEaseNodes::AllNodes;
        bool changed = false;
        const auto set = [&](float& ease, float amount) {
            amount = std::clamp(amount, 0.0f, 1.0f);
            changed = changed || ease != amount;
            ease = amount;
        };
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            auto& node = nodes[i];
            const bool first = i == 0, last = i + 1 == nodes.size();
            if (node.autoEaseOut)
                set(node.easeOut, first || (all && !last) ? start : 0.0f);
            if (node.autoEaseIn)
                set(node.easeIn, last || (all && !first) ? end : 0.0f);
        }
        return changed;
    }

    void KeepChangedEases(const Types::Keyframe& before, Types::Keyframe& after)
    {
        if (after.easeIn != before.easeIn)
            after.autoEaseIn = false;
        if (after.easeOut != before.easeOut)
            after.autoEaseOut = false;
    }

    double Retiming::GetLength() const
    {
        return lengths.empty() ? 0.0 : lengths.back();
    }

    Retiming MakeRetiming(std::span<const Types::Keyframe> nodes, std::size_t samplesPerNode)
    {
        Retiming retiming;
        if (nodes.size() < 2)
            return retiming;
        retiming.start = nodes.front().tick;
        retiming.end = nodes.back().tick;
        retiming.easeOut = nodes.front().easeOut;
        retiming.easeIn = nodes.back().easeIn;

        const std::array values = {GetValues(nodes, 0, false), GetValues(nodes, 1, false), GetValues(nodes, 2, false)};
        const auto position = [&](double tick) {
            return glm::vec3(Evaluate(nodes, values[0], tick, false), Evaluate(nodes, values[1], tick, false),
                             Evaluate(nodes, values[2], tick, false));
        };
        samplesPerNode = std::max<std::size_t>(samplesPerNode, 1);
        auto last = position(retiming.start);
        double length = 0.0;
        retiming.ticks.push_back(retiming.start);
        retiming.lengths.push_back(0.0);
        for (std::size_t i = 0; i + 1 < nodes.size(); ++i)
        {
            const double from = nodes[i].tick, to = nodes[i + 1].tick;
            for (std::size_t s = 1; s <= samplesPerNode; ++s)
            {
                const double tick = from + (to - from) * s / samplesPerNode;
                const auto here = position(tick);
                length += glm::distance(last, here);
                last = here;
                retiming.ticks.push_back(tick);
                retiming.lengths.push_back(length);
            }
        }
        return retiming;
    }

    double Retime(const Retiming& retiming, double tick)
    {
        if (retiming.ticks.size() < 2 || retiming.end <= retiming.start || tick <= retiming.start ||
            tick >= retiming.end)
            return tick;
        const double fraction =
            Ease((tick - retiming.start) / (retiming.end - retiming.start), retiming.easeOut, retiming.easeIn);
        const double total = retiming.GetLength();
        if (total <= 0.0)
            return retiming.start + fraction * (retiming.end - retiming.start);

        // the sample the camera has passed, and how far it is to the next
        const double length = fraction * total;
        const auto next = std::upper_bound(retiming.lengths.begin(), retiming.lengths.end(), length);
        if (next == retiming.lengths.end())
            return retiming.end;
        const auto j = static_cast<std::size_t>(next - retiming.lengths.begin());
        const double from = retiming.lengths[j - 1], to = retiming.lengths[j];
        const double along = to > from ? (length - from) / (to - from) : 0.0;
        return retiming.ticks[j - 1] + along * (retiming.ticks[j] - retiming.ticks[j - 1]);
    }

    double GetPassingTick(const Retiming& retiming, double pathTick)
    {
        const double total = retiming.GetLength();
        if (retiming.ticks.size() < 2 || retiming.end <= retiming.start || total <= 0.0)
            return pathTick;
        if (pathTick <= retiming.start)
            return retiming.start;
        if (pathTick >= retiming.end)
            return retiming.end;
        const auto next = std::upper_bound(retiming.ticks.begin(), retiming.ticks.end(), pathTick);
        const auto j = static_cast<std::size_t>(next - retiming.ticks.begin());
        const double along = (pathTick - retiming.ticks[j - 1]) / (retiming.ticks[j] - retiming.ticks[j - 1]);
        const double length = retiming.lengths[j - 1] + along * (retiming.lengths[j] - retiming.lengths[j - 1]);
        const double fraction = Uneased(length / total, retiming.easeOut, retiming.easeIn);
        return retiming.start + fraction * (retiming.end - retiming.start);
    }

    Types::KeyframeValue EvaluateRetimed(const Retiming& retiming, std::span<const Types::Keyframe> nodes, double tick)
    {
        return Evaluate(Types::KeyframeValueType::CameraData, nodes, Retime(retiming, tick), false);
    }

    std::optional<KeyDrag::Taken> KeyDrag::Frame(const Types::Keyframe* held)
    {
        std::optional<Taken> ended;
        if (taken && (!held || held->id != taken->id))
        {
            ended = taken;
            taken.reset();
        }
        if (held && !taken)
            taken = Taken{held->id, held->value};
        return ended;
    }

    namespace
    {
        // below anything that shows: the gizmo's own rounding moves a node 0.002 units on a click (float at 1700 units)
        constexpr float SAME_PLACE = 0.01f;
    }  // namespace

    bool IsSameCamera(const Types::CameraData& a, const Types::CameraData& b)
    {
        constexpr float AXES = 0.0001f;
        const auto turn = [](const glm::vec3& rotation) {
            return glm::mat3(glm::eulerAngleZYX(glm::radians(rotation.y), glm::radians(rotation.x),
                                                glm::radians(rotation.z)));
        };
        if (glm::distance(a.position, b.position) > SAME_PLACE || std::abs(a.fov - b.fov) > SAME_PLACE)
            return false;
        const auto x = turn(a.rotation), y = turn(b.rotation);
        for (int i = 0; i < 3; ++i)
        {
            if (glm::distance(x[i], y[i]) > AXES)
                return false;
        }
        return true;
    }

    std::string_view GetNodeDragVerb(const Types::CameraData& before, const Types::CameraData& after)
    {
        return glm::distance(before.position, after.position) > SAME_PLACE ? "move" : "turn";
    }
}  // namespace IWXMVM::KeyframeLogic
