#pragma once

namespace IWXMVM::Components::Markers
{
    // The user's own markers on the timeline (M adds one at the playhead): a tick and a name, saved with the demo's
    // keyframes. Changes are steps of the editor's undo history. All on the render thread, which draws the UI.

    struct Marker
    {
        std::uint32_t id = 0;
        std::uint32_t tick = 0;
        std::string name;

        bool operator==(const Marker&) const = default;
    };

    // by tick
    const std::vector<Marker>& GetAll();
    std::optional<Marker> Find(std::uint32_t id);

    // Adds a marker (named "Marker 3" without a name) and returns its id, as a step to undo
    std::uint32_t Add(std::uint32_t tick, std::string name = {});
    // as one step, named by the label ("delete 2 markers") unless it is empty
    void Remove(std::span<const std::uint32_t> ids, std::string label = {});
    void Rename(std::uint32_t id, std::string name);

    // A drag moves markers without a step each frame, and makes one step of it at the end: SetTick while it goes on,
    // from what Get() had before it, then PushMove with that
    void SetTick(std::uint32_t id, std::uint32_t tick);
    void PushMove(std::vector<Marker> before, std::string label);

    // markers read from a file, added to these (not a step)
    void Append(std::vector<Marker> markers);
    // when another demo loads (not a step)
    void Clear();
}  // namespace IWXMVM::Components::Markers
