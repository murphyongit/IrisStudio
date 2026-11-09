#pragma once

#include "Types/Actor.hpp"

namespace IWXMVM::UI::ViewMenu
{
    // The view's right-click menu, in the free and orbit cameras (a right-click let go at once; held, it flies or
    // orbits): for the actor clicked, an animation from the favourites and recent ones, what can follow its clip, its
    // character, walking, looking at the camera, dying, duplicate and delete; for a point of the selected actor's path,
    // deleting it, its gait from there on, a wait there, ending the path there; for the line between two of its
    // points, a point there and the stretch's gait; on the ground, adding an actor, a dog or a killstreak there and,
    // for the selected actor, walking, running or sprinting there, a point of its path there or moving it there

    // what was clicked, where in the view, and the ray through it into the world: an actor, a point of the selected
    // actor's path or the line from one (its index), else the ground
    void Open(std::optional<std::uint32_t> actor, glm::vec2 at, glm::vec3 origin, glm::vec3 direction,
              std::optional<std::size_t> point = std::nullopt, std::optional<std::size_t> line = std::nullopt);
    // in the view's window, once a frame
    void Draw();

    // The items of the menus of a point of an actor's path and of the line from one to the next, as the view has them,
    // for the timeline's path bar; there Insert point here puts the point on the ground
    // where the actor is at a tick (of the actor's time). Their items are recorded for the harness as record + their
    // name (the view's are "viewmenu.").
    void DrawPathPointMenu(const Types::Actor& actor, std::size_t point, const std::string& record);
    void DrawPathLineMenu(const Types::Actor& actor, std::size_t line, std::uint32_t insertTick,
                          const std::string& record);

    // whether it is open, and for which actor (none for the ground), point or line, for the harness
    struct State
    {
        bool open = false;
        std::optional<std::uint32_t> actor;
        std::optional<std::size_t> point;
        std::optional<std::size_t> line;
    };
    State GetState();
}  // namespace IWXMVM::UI::ViewMenu
