#pragma once

namespace IWXMVM::GFX
{
    // What the gizmo moves: one thing at a time, so a campath node can be moved on the actors tab too
    enum class GizmoTarget
    {
        None,
        CampathNode,  // the selected node
        Actor,        // the selected actor, or its selected path point
    };

    // What the gizmo and the view's picking are at, without the graphics headers (for the game bindings' tests)
    struct GizmoState
    {
        GizmoTarget target = GizmoTarget::None;
        bool shown = false;                       // drawn last frame
        std::optional<std::uint32_t> hoveredActor;
        // the selected actor's path point under the mouse, else the line from one under it (its first point's index)
        std::optional<std::size_t> hoveredPoint;
        std::optional<std::size_t> hoveredLine;
        std::optional<std::int32_t> selectedNode;
        std::string_view mode;  // "local", "world" or "rotate"
        bool overView = false;       // the mouse is over the game's picture
        bool objectHovered = false;  // and something there is under it
    };
    GizmoState GetGizmoState();
    // how strong the view's lines are now, 1 but while the camera flies
    float GetViewLineDim();
    // turns Click to place on or off for the selected actor's path (see GraphicsManager::SetPlacingPathPoints)
    void SetPlacingPathPoints(bool placing);

    // where a point of the world is drawn in the game view, in the overlay's coordinates; none behind the camera
    std::optional<glm::vec2> WorldToScreen(glm::vec3 point);
    // the ray from the camera through a point of the game view (the overlay's coordinates): its origin and direction
    std::pair<glm::vec3, glm::vec3> GetViewRay(glm::vec2 point);
}  // namespace IWXMVM::GFX
