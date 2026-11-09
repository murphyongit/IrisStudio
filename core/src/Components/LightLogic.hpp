#pragma once
#include <array>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "RuntimeProperties.hpp"
#include "Types/Actor.hpp"
#include "Types/Keyframe.hpp"

// What a light in the scene does that doesn't need the game: an actor of kind Light (see
// Types::ActorLight), on over its bar, with keys for its colour, intensity, radius and cone, a flicker worked out from
// the tick alone so that a rewind and a capture see what a playthrough did, and what it follows.
namespace IWXMVM::Components::LightLogic
{
    inline constexpr float MIN_RADIUS = 1.0f;
    inline constexpr float MAX_RADIUS = 5000.0f;
    inline constexpr float MAX_INTENSITY = 16.0f;
    // degrees across: the game's spot works out its matrices from a half angle under 90
    inline constexpr float MIN_CONE = 2.0f;
    inline constexpr float MAX_CONE = 170.0f;
    inline constexpr float MAX_FLICKER_RATE = 60.0f;
    inline constexpr float MAX_HIGHLIGHTS = 4.0f;
    // how long a bar starts, and how short one can be made
    inline constexpr std::uint32_t LIGHT_LENGTH = 8000;
    inline constexpr std::uint32_t MIN_BAR = 100;
    // how far off a surface a point lands, so it lights the surface and not only itself
    inline constexpr float POINT_OFF_SURFACE = 8.0f;

    // Keeps a light's values in their ranges and its bar a tenth of a second at least
    void Normalize(Types::ActorLight& light);
    // An id for a new light: none of the scene's lights has it
    std::uint32_t MakeId(const std::vector<Types::Actor>& scene, const std::set<std::uint32_t>& keyed = {});

    // A point POINT_OFF_SURFACE out of a surface; a spot where the camera is, aimed at the point (it lights what was
    // clicked), both on from a tick for LIGHT_LENGTH; named by the caller
    Types::Actor MakeLight(Types::LightKind kind, glm::vec3 point, glm::vec3 normal, glm::vec3 camera,
                           std::uint32_t tick);
    // Where a light that was placed by a ray goes, as MakeLight puts it, keeping its other values
    void PlaceOnSurface(Types::Actor& actor, glm::vec3 point, glm::vec3 normal, glm::vec3 camera);
    // Points it at a point, staying where it is
    void AimAt(Types::Actor& actor, glm::vec3 point);
    // What it is called in the list before its number: "Point", "Spot"
    std::string GetBaseName(Types::LightKind kind);

    // The values keys can change over time, each a property of its own per light (Components::RuntimeProperties). Its
    // place is two of them: its position (the offset from the bone while it follows one) and its aim (a spot's pitch
    // and yaw, or those from the bone).
    enum class Channel
    {
        Color,
        Intensity,
        Radius,
        Cone,
        Position,
        Aim,
    };
    // those with rows of their own on its track and page; its place has rows of its own apart from these
    inline constexpr std::array<Channel, 4> CHANNELS = {Channel::Color, Channel::Intensity, Channel::Radius,
                                                        Channel::Cone};
    inline constexpr std::array<Channel, 6> ALL_CHANNELS = {Channel::Color,  Channel::Intensity, Channel::Radius,
                                                            Channel::Cone,   Channel::Position,  Channel::Aim};
    // A property's id stays with the light's: "light/<id>/intensity"
    std::string GetPropertyId(std::uint32_t light, Channel channel);
    std::string_view GetChannelName(Channel channel);
    // Which light a property is of, by its id (GetPropertyId); none for others
    std::optional<std::uint32_t> ParseLight(std::string_view propertyId);
    RuntimeProperties::Description Describe(std::uint32_t light, Channel channel);
    Types::KeyframeValue GetValue(const Types::ActorLight& light, Channel channel);
    void SetValue(Types::ActorLight& light, Channel channel, const Types::KeyframeValue& value);
    // (its place's are the actor's as well as the light's)
    Types::KeyframeValue GetValue(const Types::Actor& actor, Channel channel);
    void SetValue(Types::Actor& actor, Channel channel, const Types::KeyframeValue& value);

    // A light's keys, by channel
    using Keys = std::array<std::vector<Types::Keyframe>, ALL_CHANNELS.size()>;
    // The light with its keyed values as they are at a tick
    Types::ActorLight ApplyKeys(Types::ActorLight light, const Keys& keys, double tick);
    // The actor with its light's place as its keys have it at a tick. Its aim turns the short way between two keys:
    // each key's yaw is taken as the one nearest the key's before it, so 170 to -170 turns 20 degrees.
    Types::Actor ApplyPlacementKeys(Types::Actor actor, const Keys& keys, double tick);
    // A yaw moved by whole turns to be as near another as it can be (190 for -170 to be near 170)
    float NearestYaw(float yaw, float to);
    // The aim's keys with each yaw nearest the one before it, as they turn
    std::vector<Types::Keyframe> UnwrapAim(std::vector<Types::Keyframe> keys);
    // An aim as a key at a tick takes it (a change keys it there): its yaw nearest the key before the tick as they
    // turn, or the key after it when there is none before, the aim's own (in -180 to 180) when there are no others
    glm::vec3 GetAimForKey(const std::vector<Types::Keyframe>& keys, std::uint32_t tick, glm::vec3 aim);

    // The selected light's path in the view: its position's keys as it goes along them, a point every 20
    // units or so, from its first key to its last; none for fewer than two keys
    std::vector<glm::vec3> GetPathLine(const std::vector<Types::Keyframe>& positionKeys);

    // The bone of the gun on screen a light on the viewed player's bone goes by in first person, nothing for
    // the body's: the gun's bones (those of the gun's model, the hand's tag_weapon_right as its j_gun) with Auto, any
    // it has (the arms' too) with View gun, never with Body
    std::optional<std::string> GetViewGunBone(Types::LightFirstPerson firstPerson, std::string_view bone,
                                              std::span<const std::string> armBones,
                                              std::span<const std::string> gunBones);

    // How much of its intensity a flicker leaves at a tick, 1 - amount to 1: smooth noise changing rate times a
    // second, the same for a tick every time, different for each light
    float GetFlicker(std::uint32_t seed, float amount, float rate, double tick);

    // The game's sRGB to the linear colour the renderer takes, as its add-light calls do
    float ToLinear(float gamma);
    glm::vec3 ToLinear(glm::vec3 color);

    // A light as the renderer wants it at a tick; none while it is off: outside its bar, hidden, or at no intensity
    struct State
    {
        Types::LightKind kind = Types::LightKind::Point;
        glm::vec3 color{0.0f};  // linear, with its intensity and flicker
        float radius = 0.0f;
        float cone = 0.0f;  // degrees across
        float softness = 0.0f;
        bool shadows = false;
        float highlights = 1.0f;

        bool operator==(const State&) const = default;
    };
    // (its bar and flicker go by the actors' tick, its keys by the timeline's)
    std::optional<State> GetState(const Types::Actor& actor, const Keys& keys, double tick, double keysTick);
    std::optional<State> GetState(const Types::Actor& actor, const Keys& keys, double tick);

    // The direction a light's pitch and the actor's yaw point it
    glm::vec3 GetAim(float pitch, float yaw);

    // Where a light is and where it points: as it was placed, or on what it follows: its offset along the bone's axes
    // (the columns: forward, left, up), a spot's aim turned from the bone's forward
    struct Placed
    {
        glm::vec3 position{0.0f};
        glm::vec3 direction{1.0f, 0.0f, 0.0f};

        bool operator==(const Placed&) const = default;
    };
    Placed Place(const Types::Actor& actor);
    Placed Place(const Types::Actor& actor, glm::vec3 bonePosition, const glm::mat3& boneAxes);

    // The rectangle of the view a light's sphere can touch, in pixels: the additive pass draws the view's surfaces once
    // per light, and only this much of them. Worked out from the corners of the cube around the sphere, so it is never
    // too small; the whole view when any of them is behind the camera.
    struct ScreenRect
    {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;

        bool operator==(const ScreenRect&) const = default;
    };
    ScreenRect GetScissor(glm::vec3 origin, float radius, const glm::mat4& viewProjection, const ScreenRect& viewport);

    // The one spot that casts shadows at a tick (the game has one shadow map for them): of the scene's lights and their
    // states, the first on that wants them; none when no spot does
    std::optional<std::uint32_t> GetShadowed(const std::vector<Types::Actor>& scene,
                                             const std::vector<std::optional<State>>& states);
    // Whether a light wants shadows and doesn't get them at a tick, as another spot has them: its row says so
    bool LosesShadows(const std::vector<Types::Actor>& scene, std::uint32_t actor, double tick);

    // What the view draws for a light while working, as lines of points: its icon, a sun that faces the camera, and
    // its reach: three rings of its radius for a point, a cone with a ring at its end for a spot
    std::vector<std::vector<glm::vec3>> GetIconLines(glm::vec3 position, glm::vec3 right, glm::vec3 up, float size);
    std::vector<std::vector<glm::vec3>> GetReachLines(Types::LightKind kind, const Placed& placed, float radius,
                                                      float cone);
}  // namespace IWXMVM::Components::LightLogic
