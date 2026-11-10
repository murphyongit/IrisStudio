#pragma once

namespace IWXMVM::Components::SkyLogic
{
    // The Look tab's Sky section, apart from the game and the device so the tests build it on their own
    // (Components::Skybox draws it). A map's sky is a cube map (Black Ops: the skybox model's material, technique set
    // mc_sky_cubemap_hdr, and the world's sky brushes, wc_sky): the sky is drawn again into a cube of the mod's, turned
    // and graded, which the game draws in place of its own. Every value at its default draws the game's own sky.
    //
    // A sky swapper (an HDRI in place of the map's sky) fits in the same place: it gives the bake another source
    // (Skybox's Source), the same turn and grade applied on top.

    struct Grade
    {
        float hue = 0.0f;         // degrees around the colour wheel
        float saturation = 1.0f;  // 0 grey, 1 the sky's own
        float brightness = 1.0f;  // times the sky's
        float contrast = 1.0f;    // around the middle grey, 1 the sky's own
        glm::vec3 tint = glm::vec3(1.0f);      // each channel times
        glm::vec3 rotation = glm::vec3(0.0f);  // yaw (around up), pitch and roll, degrees

        bool operator==(const Grade&) const = default;
    };

    // the sliders' ranges (and the keys' graphs)
    inline constexpr float MIN_HUE = -180.0f, MAX_HUE = 180.0f;
    inline constexpr float MAX_SATURATION = 3.0f;
    inline constexpr float MAX_BRIGHTNESS = 4.0f;
    inline constexpr float MAX_CONTRAST = 3.0f;
    inline constexpr float MAX_TINT = 2.0f;
    inline constexpr float MAX_ROTATION = 360.0f;  // either way: a turn and back again, keyed 0 to 360 for a whole spin
    // the grey contrast turns around, in the texture's own (gamma) values
    inline constexpr float CONTRAST_PIVOT = 0.5f;

    // whether a grade leaves the sky as the game draws it: nothing to bake, the game's own texture drawn
    bool IsGameSky(const Grade& grade);
    // whether a grade turns the sky (and not only colours it)
    bool IsTurned(const Grade& grade);

    // The grade's colours as one affine transform of a texel's red, green and blue: row i's xyz times the colour, plus
    // its w, is channel i. Hue (around grey, which it keeps), then saturation (to Rec. 709 luma), contrast, brightness
    // and tint. Alpha is left alone (the HDR skies keep a scale in it).
    std::array<glm::vec4, 3> GetColorRows(const Grade& grade);
    // a colour through those rows, as the bake's pixel shader does (negatives clamped to 0, nothing clamped above)
    glm::vec3 ApplyColor(const std::array<glm::vec4, 3>& rows, glm::vec3 color);

    // The sky's turn, in the cube's axes: the game looks up its sky cube with the world's direction (Black Ops: x
    // forward, y left, z up), so yaw turns around the cube's z. Yaw, then pitch, then roll, each counterclockwise
    // looking down its axis.
    glm::mat3 GetRotation(const Grade& grade);

    // A face of a D3D cube map (D3DCUBEMAP_FACE_POSITIVE_X...): the direction of its middle, and those its texture's u
    // (to the right) and v (down) go along, from -1 to 1 across it. A texel at (s, t) in [-1, 1] looks along
    // middle + s * right + t * down.
    struct Face
    {
        glm::vec3 middle;
        glm::vec3 right;
        glm::vec3 down;
    };
    inline constexpr std::size_t FACE_COUNT = 6;
    Face GetFace(std::size_t face);
    // The direction the bake samples the game's sky with for a face's texels: the face's axes turned back by the sky's
    // turn (the turned sky's texel at d is the game's at rotation⁻¹ × d)
    Face GetSampleFace(std::size_t face, const glm::mat3& rotation);
    // the direction of a texel (s, t in [-1, 1]) of a face, not normalized
    glm::vec3 GetDirection(const Face& face, float s, float t);
    // the face and texel coordinates (s, t in [-1, 1]) a direction looks up, as D3D picks them: the largest axis
    std::pair<std::size_t, glm::vec2> FindTexel(glm::vec3 direction);

    // the levels a baked cube of this size has: down to 1 × 1, no more than the game's texture has
    std::uint32_t GetLevelCount(std::uint32_t size, std::uint32_t sourceLevels);
}  // namespace IWXMVM::Components::SkyLogic
