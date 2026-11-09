#pragma once

namespace IWXMVM::Types
{
    enum class KeyframeablePropertyType
    {
        CampathCamera,
        SunLightColor,
        SunLightBrightness,
        SunLightDirection,
        FilmtweakBrightness,
        FilmtweakContrast,
        FilmtweakDesaturation,
        FilmtweakTintLight,
        FilmtweakTintDark,
        DepthOfFieldFarBlur,
        DepthOfFieldFarStart,
        DepthOfFieldFarEnd,
        DepthOfFieldNearBlur,
        DepthOfFieldNearStart,
        DepthOfFieldNearEnd,
        DepthOfFieldBias,

        // Added with keys for the rest of the Visuals tab (the user's list of 2026-10-24, F8); keyframe files name
        // properties, so where they are in this list doesn't matter. Switches, which a key holds on (1) or off (0)
        // until the next (KeyframeLogic::IsStepped):
        FilmtweakEnabled,
        FilmtweakInvert,
        DepthOfFieldEnabled,
        // Black Ops' filmtweaks per range (Features_FilmtweakRanges): the dark, mid and light range's in x, y and z
        FilmtweakContrastRanges,
        FilmtweakSaturationRanges,
        FilmtweakHueRanges,
        FilmtweakTemperatureRanges,
        FilmtweakBleachRanges,
        FilmtweakTintMid,
        FilmtweakMidStart,
        FilmtweakMidEnd,
        FilmtweakDarkFeather,
        FilmtweakLightFeather,

        // The Look tab's Quality section (Features_ShadowQuality): how far the sun's shadows
        // reach, in units
        ShadowDistance,
        // and Surface depth's (Features_SurfaceDepth): how deep, in inches, and the normal
        // maps' Bump strength
        SurfaceDepth,
        BumpStrength,

        // The bone camera's offset (forward, left, up), rotation (pitch, yaw, roll) and field of view
        // (Components::BoneCameraLogic::Keys)
        BoneCameraOffset,
        BoneCameraRotation,
        BoneCameraFov,

        // Properties made at run time (Components::RuntimeProperties): ReShade's values and
        // effects, ReShade and up, one type each. Past magic_enum's range, so they have no enum name and aren't among
        // its values; keyframe files name them by their id.
        ReShade = 0x10000,
    };

    enum class KeyframeValueType
    {
        FloatingPoint,
        Vector3,
        CameraData
    };

    struct KeyframeableProperty
    {
        Types::KeyframeablePropertyType type;
        std::string_view name;
        KeyframeValueType valueType;
        std::tuple<float, float> defaultValueRange;
        // what a Vector3's three values are called in the graph ("R", "Shadows"), X, Y and Z when not given
        std::array<std::string_view, 3> valueNames{};

        KeyframeableProperty(Types::KeyframeablePropertyType type, std::string_view name, KeyframeValueType valueType,
                             float defaultValueRangeMin, float defaultValueRangeMax,
                             std::array<std::string_view, 3> valueNames = {})
            : type(type),
              name(name),
              valueType(valueType),
              defaultValueRange(std::make_tuple(defaultValueRangeMin, defaultValueRangeMax)),
              valueNames(valueNames)
        {
        }

        bool operator<(const KeyframeableProperty& other) const
        {
            return name < other.name;
        }

       public:
        int32_t GetValueCount() const
        {
            // I dont really like the way this is done, I just cant really think of a better solution right now
            switch (valueType)
            {
                case Types::KeyframeValueType::FloatingPoint:
                    return 1;
                case Types::KeyframeValueType::CameraData:
                    return 7;
                case Types::KeyframeValueType::Vector3:
                    return 3;
                default:
                    throw std::runtime_error("Not implemented");
            }
        };
    };
}