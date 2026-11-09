#pragma once

// What the mod draws over the game's picture, apart from D3D so it can be tested: lines of
// one width on the screen at any distance, a campath node as a wire frame and pyramid whose shape shows its FOV and whose
// mark on top shows its roll, small rings for points and thin heads for arrows.
namespace IWXMVM::GFX::ViewLines
{
    // the dark edge adds this to a line's width, and how strong it is (AX1)
    inline constexpr float EDGE_PIXELS = 2.0f;
    inline constexpr float EDGE_ALPHA = 0.55f;
    // how strong what is behind a wall is (AY1), and everything while the camera flies (BB1)
    inline constexpr float BEHIND_WALLS = 0.3f;
    inline constexpr float WHILE_FLYING = 0.4f;
    // nothing nearer the eye than this is drawn
    inline constexpr float NEAREST = 1.0f;
    // how far a node's frame is in front of it, and never under this on the screen (AW1)
    inline constexpr float NODE_UNITS = 24.0f;
    inline constexpr float NODE_LEAST_PIXELS = 28.0f;
    // rings 8 px across, a selected point's filled (AZ1); other actors' smaller
    inline constexpr float POINT_PIXELS = 4.0f;
    inline constexpr float OTHER_POINT_PIXELS = 2.5f;
    // an arrow's head (BA1)
    inline constexpr float HEAD_LENGTH_PIXELS = 9.0f;
    inline constexpr float HEAD_WIDTH_PIXELS = 5.0f;

    // The view lines are drawn for
    struct Eye
    {
        glm::vec3 position{0.0f};
        glm::vec3 forward{1.0f, 0.0f, 0.0f};
        glm::vec3 up{0.0f, 0.0f, 1.0f};
        // how many pixels a unit is, one unit ahead
        float pixelsAtOne = 1.0f;
    };
    // for a view of a FOV (degrees across) and a width in pixels
    Eye MakeEye(glm::vec3 position, glm::vec3 forward, glm::vec3 up, float fov, float viewWidth);

    // how far ahead of the eye a point is, and how many units a pixel is there
    float GetDepth(const Eye& eye, glm::vec3 point);
    float GetUnitsPerPixel(const Eye& eye, float depth);

    // A line as strips that face the eye, so many pixels wide at every point: its two sides, point by point. What is
    // behind the eye is cut off, which can leave none, or two strips of one line.
    struct Strip
    {
        std::vector<glm::vec3> left;
        std::vector<glm::vec3> right;
    };
    std::vector<Strip> GetStrips(const Eye& eye, std::span<const glm::vec3> line, float widthPixels);

    // how far a node's frame is in front of it: NODE_UNITS, more where that is under NODE_LEAST_PIXELS on the screen
    float GetNodeSize(const Eye& eye, glm::vec3 position);

    // A campath node: lines from it to the corners of a frame at a size in front of it, as wide as its FOV makes it, the
    // frame, and a triangle on top of the frame. Its axes are unit vectors.
    struct NodeShape
    {
        std::vector<std::vector<glm::vec3>> lines;
        std::array<glm::vec3, 3> mark{};
    };
    NodeShape GetNodeShape(glm::vec3 position, glm::vec3 forward, glm::vec3 right, glm::vec3 up, float fov, float size);

    // a closed ring round a point, facing the eye, so many pixels from it; none behind the eye
    std::vector<glm::vec3> GetRing(const Eye& eye, glm::vec3 center, float radiusPixels);
    // an arrow's head, its point at the tip of a line coming from a point; none behind the eye
    std::optional<std::array<glm::vec3, 3>> GetArrowHead(const Eye& eye, glm::vec3 from, glm::vec3 tip);

    // a value going to a target by a step at most
    float StepTowards(float value, float target, float step);
}  // namespace IWXMVM::GFX::ViewLines
