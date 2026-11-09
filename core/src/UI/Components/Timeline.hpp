#pragma once
#include "UI/UIComponent.hpp"
#include "UI/TimelineMath.hpp"

namespace IWXMVM::UI
{
    // Goes to a tick of the timeline; while scrubbing (a drag), rewinding only past the rewind dead zone (a rewind
    // replays the demo)
    void Seek(std::int64_t tick, bool scrubbing);

    // The timeline under the view, in place of the old control bar and keyframe
    // editor: the transport (Freeze time, a frame back and forward, play, the time to type, the speed, loop the
    // capture range, Snap, Fit, Range), a ruler to scrub, a row of markers (kills, bomb plants, the capture range as a
    // band whose ends drag, the frozen tick, the user's markers) and the tracks: the campath's nodes, Look with a
    // track for each animated property, and the actors by type (TimelineGroups), each with its events, clips and keys.
    // What is on them is selected
    // (TimelineSelection), moved with snapping, trimmed, added, copied and deleted here, each gesture one step to
    // undo.
    class Timeline : public UIComponent
    {
       public:
        void Render() final;
        void Release() final;

        TimelineMath::View GetView() const
        {
            return view;
        }
        void SetView(TimelineMath::View shown);
        // the whole demo; the capture range with a margin
        void FitDemo();
        void FitCaptureRange();

        bool IsSnapping() const
        {
            return snapping;
        }
        bool IsLooping() const
        {
            return looping;
        }

        // A row of the timeline as it was drawn last, for the harness: "ruler", "markers", "lanes" (all that is in
        // sight of the tracks), "campath", "look", a
        // property's type ("FilmtweakBrightness"), "actor <id> events", "actor <id> clips", "actor <id> keys",
        // "group.<actors|killstreaks|effects|lights>" and its ".fold", ".eye", ".add" and ".name"
        struct Row
        {
            std::string name;
            ImVec2 min;
            ImVec2 max;
        };
        const std::vector<Row>& GetRows() const
        {
            return rows;
        }
        // where a tick is across the lanes
        float GetX(double tick) const;
        // An actor's path bar as it was drawn last, for the harness: its height and its pieces across
        // (TimelineMath::PathBar)
        struct PathBar
        {
            std::uint32_t actor = 0;
            float minY = 0.0f;
            float maxY = 0.0f;
            TimelineMath::PathBar shape;
        };
        const std::vector<PathBar>& GetPathBars() const
        {
            return pathBars;
        }
        // A piece of an actor's walk as it was drawn last, for the harness: its
        // index among the actor's pieces, its look, where it is across and up, and its ramp across (whose edges drag
        // its join's blend)
        struct WalkPiece
        {
            std::uint32_t actor = 0;
            std::size_t index = 0;
            std::string look;
            float x0 = 0.0f, x1 = 0.0f;
            float minY = 0.0f, maxY = 0.0f;
            float rampX0 = 0.0f, rampX1 = 0.0f;
        };
        const std::vector<WalkPiece>& GetWalkPieces() const
        {
            return walkPieces;
        }

       private:
        void Initialize() final;
        void RegisterCommands();

        TimelineMath::View view;
        bool snapping = true;
        bool looping = false;
        std::vector<Row> rows;
        std::vector<PathBar> pathBars;
        std::vector<WalkPiece> walkPieces;
        float lanesX0 = 0.0f;
        float lanesWidth = 1.0f;

        friend struct TimelineFrame;
    };
}  // namespace IWXMVM::UI
