#pragma once
#include "Types/Keyframe.hpp"

#include <span>

namespace IWXMVM::KeyframeLogic
{
    // How the keys of a property make its value at every tick, apart from the
    // game and the UI, so the tests in tests/actors-tests build it on their own. Keys are sorted by tick, no two at
    // one tick. A key's interpolation (Types::KeyInterpolation) is how its value goes on to the next key's; its ease
    // out slows the start of the way and the next key's ease in its end. Angles (the campath's pitch, yaw and roll)
    // are unwrapped first: each key's is taken within 180 degrees of the key's before it, so the camera turns the
    // short way round (the free camera's yaw is never wrapped, the player's view is).

    // whether a value of a type is an angle in degrees
    bool IsAngle(Types::KeyframeValueType type, std::uint32_t index);

    // Angles, each moved by whole turns to within 180 degrees of the one before it
    void Unwrap(std::span<float> degrees);

    // One value (index) of each key, unwrapped when it is an angle: what the curves go through
    std::vector<float> GetValues(std::span<const Types::Keyframe> keys, std::uint32_t index, bool angle);

    // A fraction of the way from a key to the next (0 to 1) as the first key's ease out and the next key's ease in
    // make it: the same without them; with 1 it leaves (arrives) at rest. It never goes back.
    double Ease(double fraction, float easeOut, float easeIn);
    // the fraction that Ease makes this one of
    double Uneased(double eased, float easeOut, float easeIn);

    // How fast a key's value changes under Smooth, per tick: the slope of the parabola through the keys before and
    // after it, but 0 at the first and last key and where the values turn (so the curve doesn't overshoot them), and
    // no more than three times the slope on either side (so it doesn't between them either)
    double GetSmoothSlope(std::span<const Types::Keyframe> keys, std::span<const float> values, std::size_t key);

    // The interpolation keys had before each had its own ("Auto"), as KeyframeManager had it: linear, and a cubic
    // spline through all of them (Numerical Recipes, with flat ends)
    float InterpolateLinear(std::span<const float> ticks, std::span<const float> values, float tick);
    float InterpolateCubicSpline(std::span<const float> ticks, std::span<const float> values, float tick);

    // One value of the keys at a tick: the first key's before it, the last key's after it. Unwrapped when it is an
    // angle. Without the eases when not eased (the campath at constant speed eases only its ends).
    float Evaluate(std::span<const Types::Keyframe> keys, std::uint32_t index, bool angle, double tick,
                   bool eased = true);
    // the same with the values (GetValues) worked out already
    float Evaluate(std::span<const Types::Keyframe> keys, std::span<const float> values, double tick,
                   bool eased = true);
    // Every value of the keys at a tick
    Types::KeyframeValue Evaluate(Types::KeyframeValueType type, std::span<const Types::Keyframe> keys, double tick,
                                  bool eased = true);

    // The interpolation the first key of a property's track gets: Auto for the campath's nodes (the camera goes as it
    // always did in IWXMVM), Hold for switches, Smooth for the other properties
    Types::KeyInterpolation GetDefaultInterpolation(Types::KeyframeablePropertyType type);

    // The interpolation of a key added at a tick: the key's before it (after it, when it is the first), so a key
    // added to an older file's campath doesn't change how the rest of it goes; the property's default on a track
    // without keys, and always Hold for a switch
    Types::KeyInterpolation GetNewKeyInterpolation(std::span<const Types::Keyframe> keys, std::uint32_t tick,
                                                   Types::KeyframeablePropertyType type);

    // Whether a property's keys hold their value until the next key, with no other interpolation or eases: a switch
    // (filmtweaks and depth of field on, invert, a ReShade effect's on or a checkbox of its), whose keys are 1 (on) or
    // 0 (off), or a ReShade list's whole number (Components::RuntimeProperties::Steps)
    bool IsStepped(Types::KeyframeablePropertyType type);
    // a switch's value as keys hold it: on (1) from a half up, else off (0)
    float GetStepValue(float value);
    // a stepped property's value as keys hold it: a switch's on or off, a list's nearest whole number
    float GetStepValue(Types::KeyframeablePropertyType type, float value);
    // A key as its property can have it: a stepped property's Hold, without eases, with its step value; others as they
    // are
    Types::Keyframe Normalize(Types::Keyframe key, Types::KeyframeablePropertyType type);

    // Auto Ease: the campath eases out of its first node and into its last by the amounts
    // given (0 to 1), on the sides of nodes that follow it (Keyframe::autoEaseIn, autoEaseOut); their other sides that
    // follow it have none, so a node that is no longer an end lets its ease go and the new end takes it. With all nodes
    // (6.5) every stretch between two nodes eases out of the one by the start's amount and into the other by the end's:
    // only the first node's ease in and the last's ease out, which lead nowhere, have none. Eases of the nodes' own are
    // left as they are. Nodes sorted by tick. Returns whether an ease changed.
    bool ApplyAutoEase(std::span<Types::Keyframe> nodes, float start, float end,
                       Types::AutoEaseNodes which = Types::AutoEaseNodes::FirstAndLast);
    // a node's ease changed by hand (from how it was before) is its own: that side no longer follows Auto Ease
    void KeepChangedEases(const Types::Keyframe& before, Types::Keyframe& after);

    // A campath at constant speed: the camera goes along the path its nodes make at one speed, from the first node's
    // tick to the last's, slowed only by the first node's ease out and the last node's ease in; what the nodes say
    // about the camera's angles and field of view comes with the place on the path they were at. Worked out once for
    // the nodes, from samples of the path.
    struct Retiming
    {
        double start = 0.0;
        double end = 0.0;
        std::vector<double> ticks;    // ticks of the path as its nodes time it, from start to end
        std::vector<double> lengths;  // how far along the path each is
        float easeOut = 0.0f;
        float easeIn = 0.0f;

        double GetLength() const;
    };
    // (nodes of the campath: CameraData)
    Retiming MakeRetiming(std::span<const Types::Keyframe> nodes, std::size_t samplesPerNode = 128);
    // the tick of the path, as its nodes time it, the camera is at at a tick
    double Retime(const Retiming& retiming, double tick);
    // when the camera is at a tick of the path (a node's), at constant speed
    double GetPassingTick(const Retiming& retiming, double pathTick);
    // Every value of the campath's nodes at a tick, at constant speed
    Types::KeyframeValue EvaluateRetimed(const Retiming& retiming, std::span<const Types::Keyframe> nodes, double tick);

    // One drag of a gizmo on a key, however many frames it takes: the key as it was when an axis was grabbed, given
    // back as the drag ends, so the drag is one step to undo. Each frame says which key is
    // held, as it was before that frame's gizmo moved it (the frame that grabs an axis can already turn it); none, or
    // another key, ends the drag.
    class KeyDrag
    {
       public:
        struct Taken
        {
            std::int32_t id;
            Types::KeyframeValue before;
        };
        // what the drag that ended this frame took
        std::optional<Taken> Frame(const Types::Keyframe* held);

       private:
        std::optional<Taken> taken;
    };

    // whether two values of a node have the camera in one place, looking the same way (a gizmo turns a node through a
    // quaternion, so a turn of nothing can come back as other angles: yaw 200 as -160)
    bool IsSameCamera(const Types::CameraData& a, const Types::CameraData& b);
    // what a drag did to a node, for its step's name
    std::string_view GetNodeDragVerb(const Types::CameraData& before, const Types::CameraData& after);
}  // namespace IWXMVM::KeyframeLogic
