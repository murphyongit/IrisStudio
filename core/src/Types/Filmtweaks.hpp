#pragma once
#include "glm/vec3.hpp"

namespace IWXMVM::Types
{
    struct Filmtweaks
    {
        bool enabled;
        float brightness;
        float contrast;
        float desaturation;
        glm::vec3 tintLight;
        glm::vec3 tintDark;
        bool invert;

        // Black Ops grades the dark, mid and light ranges of the image separately (x = dark, y = mid, z = light).
        // Only used by games that report Features_FilmtweakRanges; the defaults are the game's.
        glm::vec3 contrastRanges{1.0f};
        glm::vec3 saturationRanges{1.0f};
        glm::vec3 hueRanges{0.0f};
        glm::vec3 colorTemperatureRanges{6500.0f};
        glm::vec3 bleachRanges{0.0f};
        glm::vec3 tintMid{1.0f};
        float midStart = 0.25f;
        float midEnd = 0.75f;
        float darkFeather = 0.0f;
        float lightFeather = 0.0f;
    };
}  // namespace IWXMVM::Types