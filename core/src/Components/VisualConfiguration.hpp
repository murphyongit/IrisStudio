#pragma once
#include "Types/Filmtweaks.hpp"
#include "Types/Dof.hpp"
#include "Types/HudInfo.hpp"
#include "Components/SkyLogic.hpp"

namespace IWXMVM::Components
{
    class VisualConfiguration
    {
       public:
        struct Settings
        {
            // DOF
            Types::DoF dof;

            // SUN
            glm::vec3 sunColor;
            glm::vec3 sunDirection;
            float sunBrightness = 1;

            // FILMTWEAKS
            Types::Filmtweaks filmtweaks;

            // MISC
            Types::HudInfo hudInfo;

            // QUALITY: how far the sun's shadows reach, in units (Features_ShadowQuality); 0
            // in a game without
            float shadowDistance = 0.0f;
            // Surface depth (Features_SurfaceDepth): on or off, how deep (inches) and the
            // normal maps' Bump strength (1 the game's), which works on its own
            bool surfaceDepth = false;
            float surfaceDepthAmount = 1.0f;
            float bumpStrength = 1.0f;

            // SKY (Features_Sky, Components::Skybox): the map's sky turned and graded; at the defaults, the game's own
            SkyLogic::Grade sky;
        };

        static bool Load(std::filesystem::path file, Settings& visuals);
        static void Save(std::filesystem::path file, Settings settings);
    };
}