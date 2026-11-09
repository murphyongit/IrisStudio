#pragma once

// Going to the parts of a campaign level or Zombies map: G opens the level from
// above over the view, each area a number, and a click goes there and closes it; the view's menu opens it too; [ and ] go
// to the previous and next area while flying or with the map open (13, CL1 and CM1). A demo's free camera goes there,
// facing along the area, or the player of a live game, with noclip on.
namespace IWXMVM::UI::AreaMap
{
    void RegisterCommands();
    // whether the level has areas to go to
    bool HasAreas();
    void Open();
    void Close();
    // in the view's window, once a frame
    void Draw();
    void GoTo(std::size_t area);

    // for the harness
    struct State
    {
        bool open = false;
        std::size_t areas = 0;
        std::optional<std::size_t> current;  // the area gone to last
    };
    State GetState();
}  // namespace IWXMVM::UI::AreaMap
