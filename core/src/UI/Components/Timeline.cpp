#include "StdInclude.hpp"
#include "Timeline.hpp"

#include "UI/Theme.hpp"

#include <IconsFontAwesome6.h>
#include <set>
#include <imgui_internal.h>

#include "Mod.hpp"
#include "Events.hpp"
#include "Components/Actors.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Components/CameraManager.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/KeyframeSerializer.hpp"
#include "Components/KillFinder.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/LightLogic.hpp"
#include "Components/Lights.hpp"
#include "Components/PropLogic.hpp"
#include "Components/Props.hpp"
#include "Components/RuntimeProperties.hpp"
#include "Components/Markers.hpp"
#include "Components/Playback.hpp"
#include "Components/BulletTime.hpp"
#include "Components/FrozenSpans.hpp"
#include "Components/Rewinding.hpp"
#include "Components/TransitionLogic.hpp"
#include "Components/UndoHistory.hpp"
#include "Components/VisualsLogic.hpp"
#include "Graphics/Graphics.hpp"
#include "UI/ActorActions.hpp"
#include "UI/GaitColors.hpp"
#include "UI/Commands.hpp"
#include "UI/ImGuiEx/ImGuiExtensions.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/TimelineGroups.hpp"
#include "UI/TimelineSelection.hpp"
#include "UI/Toasts.hpp"
#include "UI/UIManager.hpp"
#include "UI/ViewMenu.hpp"
#include "UI/Layout.hpp"
#include "UI/Components/ActorsMenu.hpp"
#include "UI/Components/TimelineGraph.hpp"
#include "Utilities/PathUtils.hpp"

namespace IWXMVM::UI
{
    namespace
    {
        using Components::KeyframeManager;
        using TimelineSelection::Item;
        using TimelineSelection::Kind;

        // snapping reaches this far, a press moves this far before it is a drag, a clip's ends trim it this close
        constexpr float SNAP_PIXELS = 8.0f;
        constexpr float DRAG_PIXELS = 3.0f;
        constexpr float EDGE_PIXELS = 5.0f;
        // a point's tick on an actor's path bar is taken this close
        constexpr float TICK_PIXELS = 4.0f;
        // whole seconds are snapped to once they are this far apart
        constexpr float SECOND_SNAP_PIXELS = 24.0f;
        // what the snap targets of actors' paths are called (a one-off dropped on one hangs on it)
        constexpr const char* PATH_END_SNAP = "the path's end";
        constexpr const char* PATH_POINT_SNAP = "a point of its path";
        // the view shows at least this many ticks
        constexpr std::int32_t MIN_SPAN = 100;
        constexpr std::uint32_t MIN_CLIP_LENGTH = 100;
        // a join's blend dragged on its ramp goes from and to this (ticks), as the panel's
        constexpr std::int64_t MIN_DRAGGED_BLEND = 50;
        constexpr std::int64_t MAX_DRAGGED_BLEND = 1000;

        constexpr ImU32 CAPTURE_COLOR = IM_COL32(225, 55, 55, 255);
        constexpr ImU32 FROZEN_COLOR = IM_COL32(255, 230, 0, 255);
        // bullet time's spans, as the view's FROZEN strip
        constexpr ImU32 SPAN_COLOR = IM_COL32(60, 170, 200, 255);
        constexpr ImU32 MARKER_COLOR = IM_COL32(110, 205, 140, 255);
        constexpr ImU32 EVENT_COLOR = IM_COL32(255, 170, 60, 255);
        constexpr ImU32 BOMB_COLOR = IM_COL32(255, 140, 25, 255);
        constexpr ImU32 KEY_COLOR = IM_COL32(185, 185, 185, 255);
        constexpr ImU32 SNAP_COLOR = IM_COL32(255, 255, 255, 210);

        ImU32 WithAlpha(ImU32 color, int alpha)
        {
            return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
        }

        std::optional<Types::Dvar> timescale;

        // The tracks' rows drawn this frame, in screen coordinates
        enum class RowKind
        {
            Campath,
            Look,
            Property,
            ActorEvents,
            ActorClips,
            ActorFace,
            ActorKeys,
            Effect,
            Light,
            Group,
        };
        struct Lane
        {
            RowKind kind = RowKind::Campath;
            std::uint32_t owner = 0;  // the property's type, the actor's id
            ImRect rect;
        };
        std::vector<Lane> lanes;
        // (the markers row isn't a lane)
        constexpr std::size_t MARKERS_ROW = std::numeric_limits<std::size_t>::max();

        // What was drawn this frame that the mouse can take
        struct Drawn
        {
            std::vector<Item> items;  // a key of Look's summary is every key at its tick
            ImRect rect;
            std::int64_t start = 0;
            std::int64_t end = 0;
            std::size_t lane = 0;
            // a clip's ends trim it (but its end not while the next clip starts before it): where they are drawn
            bool trimStart = false;
            bool trimEnd = false;
            float startX = 0.0f;
            float endX = 0.0f;
            // a piece of a walk: its stretch, which a click selects with the walk,
            // and where a double-click goes (its join's point)
            std::optional<std::size_t> stretch;
            std::optional<std::int64_t> jump;
        };
        std::vector<Drawn> drawn;

        // the pieces of the actors' walks as drawn: each with its ramp (a joined one's edges drag its join's blend, B1)
        // and where its join's point is on the path bar
        struct PieceHandle
        {
            std::uint32_t actor = 0;
            std::size_t index = 0;  // among the actor's pieces
            ActorLogic::WalkPiece piece;
            ImRect rect;
            ImRect ramp;
            std::optional<float> pointX;
        };
        std::vector<PieceHandle> pieceHandles;

        // the capture range in the markers row: its ends and the band between them
        struct RangeHandles
        {
            ImRect start;
            ImRect end;
            ImRect band;
        };
        RangeHandles rangeHandles;

        // What the mouse does from a press until it is let go
        enum class Gesture
        {
            None,
            PressItem,  // on an item: a click, or a move once it drags
            Move,
            Trim,       // a clip's start or end
            PressLane,  // on an empty lane: a click, or a box once it drags
            Box,
            Scrub,
            Pan,
            Range,    // the capture range's start, end or both
            SpanEnd,  // a bullet time span's end
            PathEnd,  // the end of an actor's path bar: when it arrives
            // on an actor's path bar: a click selects the stretch or point as a click on the path in the view does, a
            // slide of the walk once it drags
            PressPath,
            SlidePath,
            JoinBlend,  // an edge of a join's ramp on a walk's piece: its blend (11.4 B1)
            // an end of an effect's or a light's bar, which trims it; its middle moves it as
            // any item is moved
            EffectBar,
        };
        struct Drag
        {
            Gesture gesture = Gesture::None;
            ImGuiMouseButton button = ImGuiMouseButton_Left;
            ImVec2 press;
            double pressTick = 0;
            std::vector<Item> items;
            // a click on one of several selected items selects only it, unless it is dragged
            bool collapse = false;
            // a click on what is all that is selected shows it in the panel again
            bool showAgain = false;
            // the box adds to what was selected before it
            bool additive = false;
            std::set<Item> before;
            // trimming: the actor and clip as they were (of its Face row with face), and which end
            Types::Actor actor;
            Types::ActorClip clip;
            bool face = false;
            bool trimStart = false;
            // the capture range: -1 its start, 1 its end, 0 both; as it was
            int rangeEnd = 0;
            std::pair<std::uint32_t, std::uint32_t> range;
            // a bullet time span's end: which span
            std::size_t span = 0;
            // the end of an actor's path bar: whose, and the tick it is dragged to (a click selects its last point);
            // a piece of the bar pressed
            std::uint32_t pathActor = 0;
            std::int64_t pathTick = 0;
            bool pathMoved = false;
            TimelineMath::PathPiece pathPiece;
            // sliding the walk: where it set off (the actor's tick), the tick grabbed (the set-off, a point's or the
            // end, nearest the press) and what it is; a join's ramp: its stretch and its point's tick
            std::uint32_t slideSetOff = 0;
            std::int64_t slideGrab = 0;
            std::string slideGrabName;
            std::size_t joinStretch = 0;
            std::uint32_t joinPoint = 0;
            // panning: the view as it was
            TimelineMath::View view;
            std::int64_t delta = 0;
            // an end of an effect's or a light's bar: whose, its start (-1) or its end (1), and its bar as it was
            std::uint32_t effectActor = 0;
            int effectEdge = 0;
            std::uint32_t barStart = 0;
            std::uint32_t barEnd = 0;
        };
        Drag drag;

        // what the drag going on snaps to, and what each is
        std::vector<std::int64_t> snapTargets;
        std::vector<const char*> snapNames;
        std::optional<std::int64_t> snappedTick;
        const char* snappedName = "";

        // the menu a right-click opened, and where
        enum class MenuKind
        {
            None,
            Items,
            Lane,
            Ruler,
            Span,
            Path,  // a stretch or point of an actor's path bar: the view's menus for them
        };
        MenuKind menuKind = MenuKind::None;
        std::uint32_t menuPathActor = 0;
        TimelineMath::PathPiece menuPathPiece;
        constexpr auto PATH_MENU = "##timelinePathMenu";
        Lane menuLane;
        std::uint32_t menuTick = 0;
        std::size_t menuSpan = 0;
        constexpr auto SPAN_MENU = "##timelineSpanMenu";

        // where the bullet time spans were drawn in the markers row, their bands and their ends' handles
        struct SpanHandle
        {
            ImRect band;
            ImRect end;
            std::size_t index = 0;
        };
        std::vector<SpanHandle> spanHandles;
        // where the actors' path bars were drawn: the bar, its end's handle, and its stretches
        struct PathHandle
        {
            std::uint32_t actor = 0;
            ImRect bar;
            ImRect end;
            std::vector<std::pair<ImRect, std::size_t>> stretches;
            // the bar's row, its pieces across and when it passes them
            ImRect area;
            TimelineMath::PathBar shape;
            ActorLogic::PathTiming timing;
        };
        std::vector<PathHandle> pathHandles;
        // where the scene's effects' bars were drawn, and their ends' handles
        struct EffectHandle
        {
            std::uint32_t actor = 0;
            ImRect bar;
            ImRect start;
            ImRect end;
        };
        std::vector<EffectHandle> effectHandles;
        // the lights whose track shows a row per value
        std::set<std::uint32_t> openLights;
        // the groups of rows: which are open, where each was among the tracks last, from
        // their top, and the group whose + opened its menu
        using TimelineGroups::Group;
        constexpr auto GROUP_COUNT = TimelineGroups::ALL.size();
        // (all open, however many groups: a list of four left the fifth, Lights, folded once Props came)
        std::array<bool, GROUP_COUNT> groupOpen = [] {
            std::array<bool, GROUP_COUNT> open{};
            open.fill(true);
            return open;
        }();
        std::array<std::optional<TimelineGroups::Span>, GROUP_COUNT> groupSpans;
        Group groupAdding = Group::Actors;
        bool openGroupMenu = false;
        constexpr auto GROUP_MENU = "##timelineGroupAdd";
        // the rows' names as drawn, which drag them within their group, and the one being dragged
        struct RowName
        {
            std::uint32_t actor = 0;
            Group group = Group::Actors;
            ImRect rect;
        };
        std::vector<RowName> rowNames;
        std::optional<std::uint32_t> draggedRow;
        // the actor selected last seen: one picked in a folded group opens it
        std::optional<std::uint32_t> seenPicked;
        // the piece of a path bar clicked last, drawn selected while its point is the actor's selected one
        std::optional<std::pair<std::uint32_t, TimelineMath::PathPiece>> pathClicked;
        // the transitions planned again just now, which flash (see Components::Actors::GetFlashes)
        std::vector<Components::Actors::ReplanFlash> flashes;
        constexpr auto ITEM_MENU = "##timelineItemMenu";
        constexpr auto LANE_MENU = "##timelineLaneMenu";
        constexpr auto RULER_MENU = "##timelineRulerMenu";
        constexpr auto RENAME_POPUP = "Rename marker##timelineRename";
        constexpr auto TIME_POPUP = "##timelineTime";
        constexpr auto KEYS_MENU = "##timelineKeys";
        constexpr auto CLEAR_POPUP = "Clear all keys?##timelineClear";
        constexpr auto TRACK_MENU = "##timelineAddTrack";

        char timeText[32] = {};
        char renameText[64] = {};
        std::uint32_t renamedMarker = 0;
        bool openRename = false;

        // the selection the view and the Actors tab had (actor, clip, event, face clip), to follow them when they
        // change
        std::optional<std::int32_t> seenViewNode;
        std::tuple<std::optional<std::uint32_t>, std::optional<std::uint32_t>, std::optional<std::uint32_t>,
                   std::optional<std::uint32_t>>
            seenPanel;
        std::optional<std::int32_t> seenOnlyNode;
        // what was changed last, to forget selected items an undo took away
        std::tuple<std::uint64_t, std::uint32_t, std::size_t> seenChanges;

        // which tracks show, and which groups are open
        bool lookOpen = true;
        bool boneCameraOpen = true;
        std::set<Types::KeyframeablePropertyType> shownProperties;

        // the graph area's height (once its splitter was dragged); how tall the tracks were last (the graph takes the
        // rest)
        std::optional<float> graphHeight;
        float tracksHeight = 0.0f;

        // when looping the capture range went back to its start last
        double loopedAt = -10.0;

        const Types::KeyframeableProperty& GetCampathProperty()
        {
            return KeyframeManager::Get().GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        }

        // the demo's end, later by the bullet time spans
        std::uint32_t GetEndTick()
        {
            return Components::Playback::GetTimelineEnd();
        }

        // Actors' clips, events and keys are in the world's time (the demo's), which bullet time puts later on the
        // timeline: they are drawn, snapped to and dragged in the timeline's ticks
        std::uint32_t ActorToTimeline(std::uint32_t tick)
        {
            return Components::Playback::ActorToTimelineTick(tick);
        }
        std::uint32_t TimelineToActor(std::int64_t tick)
        {
            return Components::Playback::TimelineToActorTick(
                static_cast<std::uint32_t>(std::max<std::int64_t>(tick, 0)));
        }

        // a span of bullet time at a timeline tick (or the span there longer), with a message to undo it
        void AddBulletTime(std::uint32_t tick)
        {
            const auto index = Components::BulletTime::Add(tick);
            Toasts::Show(std::format("{}: the demo stands still there while the camera goes on",
                                     Components::BulletTime::Describe(index)),
                         true);
        }

        // a frame of the capture, the step of a frame back or forward
        std::uint32_t GetFrameTicks()
        {
            const auto fps = std::max(Components::CaptureManager::Get().GetCaptureSettings().framerate, 1);
            return static_cast<std::uint32_t>(std::max(1l, std::lround(1000.0 / fps)));
        }

        // a text with each digit the font's widest: as wide as the widest text with digits in the same places
        std::string WithWidestDigits(std::string text)
        {
            const auto width = [](char digit) { return ImGui::CalcTextSize(&digit, &digit + 1).x; };
            char widest = '0';
            for (char digit = '1'; digit <= '9'; ++digit)
            {
                if (width(digit) > width(widest))
                    widest = digit;
            }
            std::ranges::replace_if(text, [](char each) { return each >= '0' && each <= '9'; }, widest);
            return text;
        }

    }  // namespace

    void Seek(std::int64_t tick, bool scrubbing)
    {
        tick = std::clamp<std::int64_t>(tick, 0, GetEndTick());
        if (Components::Rewinding::IsRewinding())
            return;
        if (Components::Playback::IsGameFrozen())
        {
            Components::Playback::SetTimelineTick(static_cast<std::uint32_t>(tick));
            return;
        }
        const auto current = static_cast<std::int64_t>(Components::Playback::GetTimelineTick());
        Components::Playback::SetTickDelta(static_cast<std::int32_t>(tick - current), !scrubbing);
    }

    namespace
    {

        // whether a property has a track: the campath always, the others with keys or once added; a light's only while
        // the light is in the scene, as the demo's file keeps its keys for a scene not loaded, with no row for them
        bool IsShown(const Types::KeyframeableProperty& property, const std::vector<Types::Keyframe>& keys)
        {
            if (Components::RuntimeProperties::IsLight(property.type))
                return !keys.empty() && Components::Lights::FindActor(property.type).has_value();
            if (Components::RuntimeProperties::IsProp(property.type))
                return !keys.empty() && Components::Props::FindActor(property.type).has_value();
            return property.type == Types::KeyframeablePropertyType::CampathCamera || !keys.empty() ||
                   shownProperties.contains(property.type);
        }

        // what a row has on the timeline
        void AppendRowItems(std::vector<Item>& items, const Types::Actor& actor)
        {
            for (const auto& clip : actor.clips)
                items.push_back({Kind::Clip, actor.id, clip.id});
            for (const auto& clip : actor.faceClips)
                items.push_back({Kind::FaceClip, actor.id, clip.id});
            for (const auto& event : actor.events)
                items.push_back({Kind::Event, actor.id, event.id});
            for (const auto& key : actor.keys)
                items.push_back({Kind::ActorKey, actor.id, key.tick});
            if (ActorLogic::GetBar(actor))
                items.push_back(TimelineSelection::BarItem(actor));
        }

        // what a row's name starts with: what it is
        const char* GetRowIcon(const Types::Actor& actor)
        {
            if (actor.mortar)
                return ICON_FA_BOMB;
            if (const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models))
            {
                switch (*killstreak)
                {
                    case Types::Killstreak::RcXd:
                        return ICON_FA_CAR;
                    case Types::Killstreak::CarePackage:
                        return ICON_FA_BOX;
                    case Types::Killstreak::SpyPlane:
                    case Types::Killstreak::Napalm:
                    case Types::Killstreak::RollingThunder:
                        return ICON_FA_PLANE;
                    case Types::Killstreak::SentryGun:
                    case Types::Killstreak::SamTurret:
                        return ICON_FA_CROSSHAIRS;
                    default:
                        return ICON_FA_HELICOPTER;
                }
            }
            return Components::Actors::GetActorKind(actor) == Types::ActorKind::Dog ? ICON_FA_DOG : ICON_FA_PERSON;
        }

        const char* GetGroupIcon(Group group)
        {
            switch (group)
            {
                case Group::Actors:
                    return ICON_FA_PERSON;
                case Group::Killstreaks:
                    return ICON_FA_HELICOPTER;
                case Group::Effects:
                    return ICON_FA_WAND_MAGIC_SPARKLES;
                case Group::Props:
                    return ICON_FA_CUBE;
                case Group::Lights:
                    return ICON_FA_LIGHTBULB;
            }
            return "";
        }

        // Every row of a group hidden, or shown when all are, as one step
        void ToggleRowsHidden(Group group, std::span<const Types::Actor* const> rows)
        {
            const bool hide = !std::ranges::all_of(rows, [](const Types::Actor* row) { return row->hidden; });
            const auto label = std::format("{} {}", hide ? "hide" : "show", TimelineGroups::GetName(group));
            Components::UndoHistory::Get().BeginGroup(label);
            for (const auto* row : rows)
            {
                auto changed = *row;
                changed.hidden = hide;
                Components::Actors::Update(changed, false, label);
            }
            Components::UndoHistory::Get().EndGroup();
            Components::Actors::EndEditStep();
        }

        // the capture range as one step to undo
        void PushCaptureRange(std::pair<std::uint32_t, std::uint32_t> before,
                              std::pair<std::uint32_t, std::uint32_t> after, std::string label)
        {
            if (before == after)
                return;
            const auto set = [](std::pair<std::uint32_t, std::uint32_t> ticks) {
                auto& settings = Components::CaptureManager::Get().GetCaptureSettings();
                settings.startTick = ticks.first;
                settings.endTick = ticks.second;
            };
            Components::UndoHistory::Get().Push(
                std::move(label), [set, before] { set(before); }, [set, after] { set(after); });
        }

        std::pair<std::uint32_t, std::uint32_t> GetCaptureRange()
        {
            const auto& settings = Components::CaptureManager::Get().GetCaptureSettings();
            return {settings.startTick, settings.endTick};
        }

        // Changes the capture range's start or end to a tick, as one step to undo (B and N)
        void SetCaptureRangeEnd(bool start, std::uint32_t tick)
        {
            auto& settings = Components::CaptureManager::Get().GetCaptureSettings();
            const auto before = GetCaptureRange();
            auto& end = start ? settings.startTick : settings.endTick;
            if (end == tick)
                return;
            end = tick;
            PushCaptureRange(before, GetCaptureRange(), start ? "set the capture start" : "set the capture end");
            Toasts::Show(std::format("Capture range {} at {}", start ? "starts" : "ends", Toasts::FormatTime(tick)),
                         true);
        }

        // A key of a property at a tick, with the value there (the camera's when the campath has no nodes yet)
        void AddKeyAt(const Types::KeyframeableProperty& property, std::uint32_t tick)
        {
            auto& keyframeManager = KeyframeManager::Get();
            auto& keys = keyframeManager.GetKeyframes(property);
            const bool campath = property.type == Types::KeyframeablePropertyType::CampathCamera;
            if (std::ranges::find(keys, tick, &Types::Keyframe::tick) != keys.end())
            {
                Toasts::Show(
                    std::format("There's already a {} at {}", campath ? "node" : "key", Toasts::FormatTime(tick)));
                return;
            }
            // (on a property without keys, what it is set to)
            auto value = keyframeManager.GetValueAt(property, tick);
            if (campath && keys.empty())
            {
                const auto& camera = Components::CameraManager::Get().GetActiveCamera();
                value = Types::CameraData{camera->GetPosition(), camera->GetRotation(), camera->GetFov()};
            }
            const Types::Keyframe key(property, tick, value);
            keyframeManager.AddKeyframe(property, key);
            keyframeManager.SortAndSaveKeyframes(keys);
            const auto item = TimelineSelection::KeyItem(property.type, key.id);
            TimelineSelection::Set(std::span(&item, 1));
            Toasts::Show(campath ? std::format("Node {} added at {}", keys.size(), Toasts::FormatTime(tick))
                                 : std::format("Key added at {}", Toasts::FormatTime(tick)),
                         true);
        }

        void AddMarkerAt(std::uint32_t tick)
        {
            const auto& markers = Components::Markers::GetAll();
            if (std::ranges::find(markers, tick, &Components::Markers::Marker::tick) != markers.end())
            {
                Toasts::Show(std::format("There's already a marker at {}", Toasts::FormatTime(tick)));
                return;
            }
            const Item item{Kind::Marker, 0, Components::Markers::Add(tick)};
            TimelineSelection::Set(std::span(&item, 1));
            Toasts::Show(std::format("Marker added at {}", Toasts::FormatTime(tick)), true);
        }

        // a new clip of an actor at a tick: the animation of the clip before (or the actor's first, else its body's
        // idle), up to the next clip or for five seconds
        Types::ActorClip MakeClipAt(const Types::Actor& actor, std::uint32_t tick)
        {
            Types::ActorClip clip{
                .animation = Components::Actors::GetIdleAnimation(actor), .start = tick, .length = 5000};
            const Types::ActorClip* before = nullptr;
            for (const auto& other : actor.clips)
            {
                if (other.start <= tick)
                    before = &other;
                else
                    clip.length = std::min(clip.length, other.start - tick);
            }
            if (!before && !actor.clips.empty())
                before = &actor.clips.front();
            if (before)
            {
                clip.animation = before->animation;
                clip.loop = before->loop;
            }
            clip.length = std::max(clip.length, MIN_CLIP_LENGTH);
            return clip;
        }

        // A new clip of an actor's Face row at a tick: the animation of the face clip before (or its first, else the
        // alert face), up to the next face clip or for three seconds
        Types::ActorClip MakeFaceClipAt(const Types::Actor& actor, std::uint32_t tick)
        {
            Types::ActorClip clip{.animation = std::string(ActorLogic::FACE_ALERT),
                                  .start = tick,
                                  .length = 3000,
                                  .blendIn = ActorLogic::FACE_BLEND};
            const Types::ActorClip* before = nullptr;
            for (const auto& other : actor.faceClips)
            {
                if (other.start <= tick)
                    before = &other;
                else
                    clip.length = std::min(clip.length, other.start - tick);
            }
            if (!before && !actor.faceClips.empty())
                before = &actor.faceClips.front();
            if (before)
            {
                clip.animation = before->animation;
                clip.loop = before->loop;
            }
            clip.length = std::max(clip.length, MIN_CLIP_LENGTH);
            return clip;
        }

        // whether an actor's lanes have its Face row: with a face of its own, or clips on it; not a dog's (the face's
        // animations are a person's)
        bool HasFaceRow(const Types::Actor& actor)
        {
            return (actor.face == Types::ActorFace::Auto || !actor.faceClips.empty()) &&
                   Components::Actors::GetActorKind(actor) != Types::ActorKind::Dog;
        }

        const char* DescribeEventIcon(Types::ActorEventType type)
        {
            switch (type)
            {
                case Types::ActorEventType::Fire:
                    return ICON_FA_GUN;
                case Types::ActorEventType::Hit:
                    return ICON_FA_DROPLET;
                case Types::ActorEventType::Effect:
                    return ICON_FA_WAND_MAGIC_SPARKLES;
                case Types::ActorEventType::Show:
                    return ICON_FA_EYE;
                case Types::ActorEventType::Hide:
                    return ICON_FA_EYE_SLASH;
                case Types::ActorEventType::Look:
                    return ICON_FA_CROSSHAIRS;
            }
            return "";
        }

        const char* DescribeEventName(Types::ActorEventType type)
        {
            switch (type)
            {
                case Types::ActorEventType::Fire:
                    return "Fire";
                case Types::ActorEventType::Hit:
                    return "Hit";
                case Types::ActorEventType::Effect:
                    return "Effect";
                case Types::ActorEventType::Show:
                    return "Show";
                case Types::ActorEventType::Hide:
                    return "Hide";
                case Types::ActorEventType::Look:
                    return "Look";
            }
            return "";
        }

        void DrawDiamond(ImDrawList* drawList, ImVec2 center, float radius, ImU32 fill, std::optional<ImU32> outline)
        {
            const ImVec2 top(center.x, center.y - radius), right(center.x + radius, center.y),
                bottom(center.x, center.y + radius), left(center.x - radius, center.y);
            drawList->AddQuadFilled(top, right, bottom, left, fill);
            if (outline)
                drawList->AddQuad(top, right, bottom, left, *outline, 1.5f);
        }

        // A key's colours: selected, hovered or neither
        ImU32 GetKeyFill(bool selected, bool hovered)
        {
            if (selected)
                return ImGui::GetColorU32(ImGuiCol_CheckMark);
            return hovered ? IM_COL32_WHITE : KEY_COLOR;
        }

        // Puts what is selected alone in the Actors tab (a clip, an event, the actor of a key) or the view (a node)
        void ShowInPanel(const Item& item)
        {
            if (item.kind == Kind::Clip || item.kind == Kind::Event || item.kind == Kind::ActorKey ||
                item.kind == Kind::FaceClip)
            {
                Components::Actors::Select(item.owner);
                if (item.kind == Kind::Clip)
                    Components::Actors::SelectClip(static_cast<std::uint32_t>(item.id));
                if (item.kind == Kind::FaceClip)
                    Components::Actors::SelectFaceClip(static_cast<std::uint32_t>(item.id));
                if (item.kind == Kind::Event)
                    Components::Actors::SelectEvent(static_cast<std::uint32_t>(item.id));
                UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
            }
            if (TimelineSelection::IsBar(item))
            {
                Components::Actors::Select(item.owner);
                UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
            }
            // (a key of a light's or a prop's: the light or prop)
            if (item.kind == Kind::Key)
            {
                const auto type = static_cast<Types::KeyframeablePropertyType>(item.owner);
                const auto owner = Components::Lights::FindActor(type);
                if (const auto actor = owner ? owner : Components::Props::FindActor(type))
                {
                    Components::Actors::Select(*actor);
                    UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
                }
            }
            seenPanel = {Components::Actors::GetSelected(), Components::Actors::GetSelectedClip(),
                         Components::Actors::GetSelectedEvent(), Components::Actors::GetSelectedFaceClip()};
        }

        // Selects what was clicked on an actor's path bar as a click on its path in the view does: the actor and the
        // point (a stretch's first), the gizmo on it, the Actors tab's Movement section showing it; the timeline's
        // items are let go, so Delete and Ctrl+D act on the point
        void SelectPathPiece(std::uint32_t actor, TimelineMath::PathPiece piece)
        {
            TimelineSelection::Clear();
            Components::Actors::Select(actor);
            Components::Actors::SelectWaypoint(piece.index);
            GFX::GraphicsManager::Get().FocusActor();
            pathClicked = std::pair(actor, piece);
            UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);
            // (shown again when it was selected already)
            UIManager::Get().GetUIComponent<ActorsMenu>(Component::ActorsMenu)->ShowPage(ActorsMenu::Page::Movement);
        }

        // A walk's piece clicked, its walk selected already: the point its stretches
        // start at selected too, and the Actors tab's Animation section showing the piece
        void SelectPiece(std::uint32_t actor, std::size_t stretch)
        {
            Components::Actors::Select(actor);
            Components::Actors::SelectWaypoint(stretch);
            pathClicked.reset();
            UIManager::Get().GetUIComponent<ActorsMenu>(Component::ActorsMenu)->ShowPage(ActorsMenu::Page::Animation);
        }

        // when the actor sets off along a stretch, reaches a point (where it waits there), or arrives at the end
        std::optional<std::uint32_t> GetPathPieceTick(const PathHandle& handle, TimelineMath::PathPiece piece)
        {
            using PieceKind = TimelineMath::PathPiece::Kind;
            const auto& timing = handle.timing;
            if (piece.kind == PieceKind::End)
                return timing.arrival;
            if (piece.kind == PieceKind::Stretch)
                return piece.index < timing.left.size() ? timing.left[piece.index] : std::nullopt;
            if (piece.index < timing.reached.size() && timing.reached[piece.index])
                return timing.reached[piece.index];
            return piece.index < timing.left.size() ? timing.left[piece.index] : std::nullopt;
        }

        // A click on what is selected already shows it again, as a click in the view does: its tab, and a clip's or
        // event's page, also after the user went to another tab since (the panel only follows changes of the
        // selection, see PrimaryTabs::FollowPicks)
        void ShowAgain(std::span<const Item> items)
        {
            if (items.size() == 1)
                ShowInPanel(items.front());
            std::vector<PanelLogic::Kind> kinds;
            for (const auto& item : items)
                kinds.push_back(TimelineSelection::GetPanelKind(item));
            UIManager::Get().ShowPicked(kinds);
        }

        // The selection follows the view and the Actors tab when they change what they have selected, and they
        // follow it when a single node is selected here
        void FollowSelections()
        {
            auto& graphics = GFX::GraphicsManager::Get();
            const auto viewNode = graphics.GetGizmoTarget() == GFX::GizmoTarget::CampathNode
                                      ? graphics.GetSelectedNodeId()
                                      : std::nullopt;
            if (viewNode != seenViewNode)
            {
                seenViewNode = viewNode;
                if (viewNode)
                {
                    const auto item =
                        TimelineSelection::KeyItem(Types::KeyframeablePropertyType::CampathCamera, *viewNode);
                    TimelineSelection::Set(std::span(&item, 1));
                    TimelineSelection::SetAnchor(item);
                }
            }

            const auto panel =
                std::tuple(Components::Actors::GetSelected(), Components::Actors::GetSelectedClip(),
                           Components::Actors::GetSelectedEvent(), Components::Actors::GetSelectedFaceClip());
            if (panel != seenPanel)
            {
                const auto [actor, clip, event, faceClip] = panel;
                const auto [seenActor, seenClip, seenEvent, seenFaceClip] = seenPanel;
                seenPanel = panel;
                std::optional<Item> picked;
                if (actor && clip && (clip != seenClip || actor != seenActor))
                    picked = Item{Kind::Clip, *actor, *clip};
                else if (actor && faceClip && (faceClip != seenFaceClip || actor != seenActor))
                    picked = Item{Kind::FaceClip, *actor, *faceClip};
                else if (actor && event && (event != seenEvent || actor != seenActor))
                    picked = Item{Kind::Event, *actor, *event};
                if (picked)
                {
                    TimelineSelection::Set(std::span(&*picked, 1));
                    TimelineSelection::SetAnchor(picked);
                }
            }

            const auto onlyNode = TimelineSelection::GetOnlyNode();
            if (onlyNode != seenOnlyNode)
            {
                seenOnlyNode = onlyNode;
                if (onlyNode && onlyNode != viewNode)
                {
                    graphics.SelectNode(*onlyNode);
                    seenViewNode = onlyNode;
                }
            }

            // (an undo can take selected keys away)
            const auto changes = std::tuple(Components::UndoHistory::Get().GetLatest(),
                                            Components::Actors::GetGeneration(), Components::Markers::GetAll().size());
            if (changes != seenChanges)
            {
                seenChanges = changes;
                if (!TimelineSelection::IsMoving())
                    TimelineSelection::Prune();
            }
        }

        // What a drag can snap to (but not what it moves, nor an actor that slides): the playhead, the keys of the
        // tracks shown, clips' ends, events, kills, the capture range's ends, the frozen tick, markers and whole
        // seconds
        void CollectSnapTargets(const TimelineMath::Axis& axis, const std::set<Item>& moving, bool playhead,
                                bool rangeStart = true, bool rangeEnd = true,
                                std::optional<std::uint32_t> sliding = std::nullopt)
        {
            snapTargets.clear();
            snapNames.clear();
            const auto add = [](std::int64_t tick, const char* name) {
                snapTargets.push_back(tick);
                snapNames.push_back(name);
            };
            if (playhead)
                add(Components::Playback::GetTimelineTick(), "the playhead");
            for (const auto& [property, keys] : KeyframeManager::Get().GetKeyframes())
            {
                if (!IsShown(property, keys))
                    continue;
                const bool campath = property.type == Types::KeyframeablePropertyType::CampathCamera;
                // (a light's or a prop's go with its bar)
                const auto light = Components::Lights::FindActor(property.type);
                if (light && moving.contains({Kind::LightBar, *light, 0}))
                    continue;
                const auto prop = Components::Props::FindActor(property.type);
                if (prop && moving.contains({Kind::PropBar, *prop, 0}))
                    continue;
                for (const auto& key : keys)
                {
                    if (!moving.contains(TimelineSelection::KeyItem(property.type, key.id)))
                        add(key.tick, campath ? "a node" : "a key");
                }
            }
            for (const auto& actor : Components::Actors::GetActors())
            {
                if (actor.id == sliding)
                    continue;
                for (const auto& clip : actor.clips)
                {
                    if (moving.contains({Kind::Clip, actor.id, clip.id}))
                        continue;
                    add(ActorToTimeline(clip.start), "a clip's start");
                    add(ActorToTimeline(clip.End()), "a clip's end");
                }
                for (const auto& clip : actor.faceClips)
                {
                    if (moving.contains({Kind::FaceClip, actor.id, clip.id}))
                        continue;
                    add(ActorToTimeline(clip.start), "a face clip's start");
                    add(ActorToTimeline(clip.End()), "a face clip's end");
                }
                for (const auto& event : actor.events)
                {
                    if (!moving.contains({Kind::Event, actor.id, event.id}))
                        add(ActorToTimeline(event.tick), "an event");
                }
                for (const auto& key : actor.keys)
                {
                    if (!moving.contains({Kind::ActorKey, actor.id, key.tick}))
                        add(ActorToTimeline(key.tick), "a key");
                }
                // its path's end and points: a one-off dropped on one hangs on it
                if (actor.movement == Types::ActorMovement::Path && actor.path.size() >= 2 && !actor.stretches.empty())
                {
                    const auto timing = Components::Actors::GetPathTiming(actor);
                    if (timing.arrival)
                        add(ActorToTimeline(*timing.arrival), PATH_END_SNAP);
                    for (std::size_t i = 1; i + 1 < timing.reached.size(); ++i)
                        if (timing.reached[i])
                            add(ActorToTimeline(*timing.reached[i]), PATH_POINT_SNAP);
                }
            }
            for (const auto& entry : Components::KillFinder::GetEntries())
            {
                if (Components::KillFinder::IsShown(entry))
                    add(Components::Playback::ToTimelineTick(Components::KillFinder::GetKill(entry).tick), "a kill");
            }
            const auto [start, end] = GetCaptureRange();
            if (rangeStart)
                add(start, "the capture start");
            if (rangeEnd)
                add(end, "the capture end");
            if (const auto frozen = Components::Playback::GetFrozenTick())
                add(*frozen, "the frozen tick");
            const auto spans = Components::Playback::GetFrozenSpans();
            for (std::size_t i = 0; i < spans->size(); ++i)
            {
                const auto start = Components::FrozenSpans::GetStart(*spans, i);
                add(start, "bullet time's start");
                add(start + (*spans)[i].length, "bullet time's end");
            }
            for (const auto& marker : Components::Markers::GetAll())
            {
                if (!moving.contains({Kind::Marker, 0, marker.id}))
                    add(marker.tick, "a marker");
            }
            if (axis.ToX(1000) - axis.ToX(0) >= SECOND_SNAP_PIXELS)
            {
                for (std::int64_t second = axis.view.start / 1000 * 1000; second <= axis.view.end; second += 1000)
                    add(second, "a whole second");
            }
        }

        // A one-off clip dragged alone onto its path's end or one of its points (snapped there): hangs on it there, as
        // part of the move's step
        void AnchorDropped(std::int64_t snapped)
        {
            const auto& selection = TimelineSelection::Get();
            if (selection.size() != 1 || selection.begin()->kind != Kind::Clip)
                return;
            const auto item = *selection.begin();
            auto actor = Components::Actors::GetActor(item.owner);
            if (!actor)
                return;
            const auto clip =
                std::ranges::find(actor->clips, static_cast<std::uint32_t>(item.id), &Types::ActorClip::id);
            if (clip == actor->clips.end() || clip->chain != Types::ActorChain::None ||
                ActorToTimeline(clip->start) != snapped)
                return;
            const auto timing = Components::Actors::GetPathTiming(*actor);
            if (timing.arrival && ActorToTimeline(*timing.arrival) == snapped)
                clip->anchor = Types::ActorAnchor::End;
            else
            {
                const auto point = std::ranges::find_if(timing.reached, [&](const std::optional<std::uint32_t>& tick) {
                    return tick && ActorToTimeline(*tick) == snapped;
                });
                if (point == timing.reached.end())
                    return;
                clip->anchor = Types::ActorAnchor::Point;
                clip->anchorPoint = static_cast<std::uint32_t>(point - timing.reached.begin());
            }
            clip->anchorOffset = 0;
            Components::Actors::Update(*actor, true);
        }

        bool IsSnappingNow(const Timeline& timeline)
        {
            return timeline.IsSnapping() && !ImGui::GetIO().KeyAlt;
        }

        // a tick snapped to the nearest target in reach, as the drag going on shows it
        std::int64_t SnapTick(const Timeline& timeline, const TimelineMath::Axis& axis, std::int64_t tick)
        {
            snappedTick.reset();
            if (!IsSnappingNow(timeline))
                return tick;
            const auto target = TimelineMath::Snap(tick, snapTargets, axis.TicksPer(SNAP_PIXELS));
            if (!target)
                return tick;
            snappedTick = snapTargets[*target];
            snappedName = snapNames[*target];
            return *snappedTick;
        }

        std::string DescribeSeconds(std::int64_t ticks)
        {
            return std::format("{}{:.3f} s", ticks < 0 ? "-" : "+", std::abs(ticks) / 1000.0);
        }
    }  // namespace

    // What one frame of the timeline draws and does
    struct TimelineFrame
    {
        Timeline& timeline;
        TimelineMath::Axis axis;
        std::uint32_t endTick = 0;
        std::uint32_t currentTick = 0;
        float rowHeight = 0.0f;
        float smallRow = 0.0f;
        float headerX = 0.0f;
        ImRect ruler;
        ImRect markersRow;
        // what shows of the lanes, and what of that takes the mouse: not behind a group's header at the top
        ImRect lanesView;
        ImRect lanesClip;
        bool rulerHovered = false;
        bool markersHovered = false;
        bool lanesHovered = false;
        TimelineMath::RulerSpacing spacing;

        void Run();
        void DrawTransport();
        void DrawTransportPopups();
        void DrawRuler();
        void DrawMarkersRow();
        void DrawTracks(ImVec2 size);
        ImRect AddLane(RowKind kind, std::uint32_t owner, float top, float height, std::string name,
                       bool background = true);
        void DrawLines(const ImRect& rect);
        void DrawKeys(const Types::KeyframeableProperty& property, const std::vector<Types::Keyframe>& keys,
                      const ImRect& rect, std::size_t lane);
        // (with a name of its own under a light's track, where the light's name isn't needed)
        float DrawKeyTrack(const Types::KeyframeableProperty& property, float y, float indent,
                           std::optional<std::string_view> label = {});
        // a group of tracks under the campath, the bone camera's keys' or the Look's
        float DrawTrackGroup(float y, bool boneCamera);
        float DrawActorTrack(const Types::Actor& actor, float y);
        float DrawEffectTrack(const Types::Actor& actor, float y);
        float DrawLightTrack(const Types::Actor& actor, float y);
        float DrawPropTrack(const Types::Actor& actor, float y);
        // A group's header: its arrow, name and count, an eye and a +; folded, a thin bar
        // for each row's time, which take everything in the group. Its places, what the mouse does there, how it looks.
        struct GroupParts
        {
            ImRect header;
            ImRect lane;
            ImRect arrow;
            ImRect eye;
            ImRect add;
        };
        GroupParts GetGroupParts(float y) const;
        void DrawGroupHits(Group group, std::span<const Types::Actor* const> rows, const GroupParts& parts);
        void DrawGroupHeader(Group group, std::span<const Types::Actor* const> rows, const GroupParts& parts);
        void DrawGroupSummary(Group group, std::span<const Types::Actor* const> rows, const ImRect& row, std::size_t lane);
        float DrawGroup(Group group, std::span<const Types::Actor* const> rows, float y, bool pinned);
        void DrawGroupMenu();
        // the name of a row, drawn last: it drags the row within its group
        void AddRowName(const Types::Actor& actor);
        void HandleRowDrag();
        // clips in a row of an actor's lanes: its clips or those of its Face row (compact, with small names)
        void DrawClips(const Types::Actor& actor, const std::vector<Types::ActorClip>& clips, Kind kind,
                       const ImRect& row, std::size_t lane, bool compact);
        // a walk of the path in its pieces
        void DrawWalkPieces(const Types::Actor& actor, const Types::ActorClip& clip,
                            const std::vector<ActorLogic::WalkPiece>& pieces, const ImRect& row, std::size_t lane);
        // a track's name in its header, cut off at right (before its buttons)
        void DrawHeaderName(float y, float height, float indent, const char* text, std::optional<float> right = {});
        bool DrawHeaderButton(float y, float height, const char* label, const char* tooltip);
        void HandleInput();
        void HandleAnimationDrop();
        void DrawTooltip(const Drawn& item);
        void Press(std::optional<std::size_t> hovered, std::optional<std::size_t> lane);
        void UpdateDrag();
        void Release();
        void SelectRange(const Drawn& clicked);
        // whether a point is on the playhead's line, which can be grabbed
        bool OnPlayhead(ImVec2 point) const
        {
            return std::abs(point.x - std::floor(axis.ToX(currentTick))) <= 3.0f;
        }
        // the walk's piece whose join's ramp has an edge at a point (in the lower half of the clips' row, where the
        // ramp is drawn), and which edge (0 its start)
        std::optional<std::pair<const PieceHandle*, int>> RampEdgeAt(ImVec2 point) const
        {
            // (a row scrolled out of sight is still where the ruler is on the screen, and it takes no clicks: this and
            // what follows look at the lanes alone)
            if (!lanesHovered)
                return std::nullopt;
            for (const auto& handle : pieceHandles)
            {
                if (!handle.piece.joined || point.y < (handle.rect.Min.y + handle.rect.Max.y) / 2 ||
                    point.y > handle.rect.Max.y)
                    continue;
                if (std::abs(point.x - handle.ramp.Min.x) <= EDGE_PIXELS)
                    return std::pair(&handle, 0);
                if (std::abs(point.x - handle.ramp.Max.x) <= EDGE_PIXELS)
                    return std::pair(&handle, 1);
            }
            return std::nullopt;
        }
        // the walk's last piece of an actor whose right edge is at a point (just inside it: a clip after it trims from
        // its own side), which drags when the actor arrives as the path bar's end does
        const PieceHandle* WalkEndAt(ImVec2 point) const
        {
            if (!lanesHovered)
                return nullptr;
            for (auto handle = pieceHandles.rbegin(); handle != pieceHandles.rend(); ++handle)
            {
                // (each actor's last: the one drawn last of its pieces)
                if (std::any_of(pieceHandles.rbegin(), handle,
                                [&](const PieceHandle& later) { return later.actor == handle->actor; }))
                    continue;
                if (point.y >= handle->rect.Min.y && point.y <= handle->rect.Max.y &&
                    point.x >= handle->rect.Max.x - EDGE_PIXELS && point.x <= handle->rect.Max.x + 1.0f)
                    return &*handle;
            }
            return nullptr;
        }
        // the piece of an actor's path bar at a point, and the handle of its bar
        std::optional<std::pair<const PathHandle*, TimelineMath::PathPiece>> PathPieceAt(ImVec2 point) const
        {
            if (!lanesHovered)
                return std::nullopt;
            for (const auto& handle : pathHandles)
            {
                if (!handle.area.Contains(point))
                    continue;
                if (const auto piece = TimelineMath::PickPathBar(handle.shape, point.x, TICK_PIXELS))
                    return std::pair(&handle, *piece);
            }
            return std::nullopt;
        }
        void DrawMenus();
        // the interpolation and eases of the keys selected, in the menu of the selection
        void DrawKeyOptions();
        void DrawOverlays();
        // the graph area under the tracks, and the bar above it that sets its height
        void DrawGraph(const ImRect& rect);
        void DrawGraphSplitter(float top, float height, float available);
        // the item drawn last is a row, for the harness
        void AddRow(std::string name)
        {
            timeline.rows.push_back({std::move(name), ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
        }
    };

    void TimelineFrame::Run()
    {
        FollowSelections();
        endTick = GetEndTick();
        currentTick = Components::Playback::GetTimelineTick();
        rowHeight = ImGui::GetFrameHeight();
        smallRow = ImGui::GetFontSize() * 0.75f + 4.0f;
        timeline.rows.clear();
        timeline.pathBars.clear();
        timeline.walkPieces.clear();
        lanes.clear();
        drawn.clear();

        const bool capturing = Components::CaptureManager::Get().IsCapturing();
        ImGui::BeginDisabled(capturing);
        DrawTransport();

        auto window = ImGui::GetCurrentWindow();
        const auto& style = ImGui::GetStyle();
        const auto headerWidth = std::max(ImGui::GetFontSize() * 13.0f, window->WorkRect.GetWidth() * 0.14f);
        headerX = window->WorkRect.Min.x;
        axis.view = timeline.view;
        axis.x0 = std::floor(headerX + headerWidth);
        axis.width = std::max(window->WorkRect.Max.x - style.ScrollbarSize - axis.x0, 1.0f);
        timeline.lanesX0 = axis.x0;
        timeline.lanesWidth = axis.width;
        spacing =
            TimelineMath::GetRulerSpacing(axis.TicksPer(1.0f), ImGui::CalcTextSize("0:00.00").x + ImGui::GetFontSize());

        const auto mouse = ImGui::GetMousePos();
        auto y = ImGui::GetCursorScreenPos().y;
        ruler = ImRect(ImVec2(axis.x0, y), ImVec2(axis.x0 + axis.width, y + std::floor(ImGui::GetFontSize() * 1.3f)));
        markersRow = ImRect(ImVec2(axis.x0, ruler.Max.y + 2), ImVec2(ruler.Max.x, ruler.Max.y + 2 + smallRow + 4));
        rulerHovered = !capturing && ImGui::IsWindowHovered() && ruler.Contains(mouse);
        markersHovered = !capturing && ImGui::IsWindowHovered() && markersRow.Contains(mouse);
        timeline.rows.push_back({"ruler", ruler.Min, ruler.Max});
        timeline.rows.push_back({"markers", markersRow.Min, markersRow.Max});
        DrawRuler();
        DrawMarkersRow();

        y = markersRow.Max.y + style.ItemSpacing.y;
        ImGui::SetCursorScreenPos(ImVec2(window->WorkRect.Min.x, y));
        const auto available = std::max(window->WorkRect.Max.y - y, rowHeight);
        if (capturing)
        {
            ImGui::Dummy(ImVec2(1, 1));
        }
        else if (TimelineGraph::IsOpen())
        {
            // the graph under the tracks, in the room they don't take, leaving them two rows at least
            const auto splitter = 6.0f;
            const auto fewest = rowHeight * 2 + style.ItemSpacing.y;
            const auto most = std::max(available - fewest - splitter, 40.0f);
            const auto height = std::clamp(graphHeight.value_or(available - std::max(tracksHeight, fewest) - splitter),
                                           std::min(ImGui::GetFontSize() * 6, most), most);
            const auto tracks = std::max(available - height - splitter, rowHeight);
            DrawTracks(ImVec2(0.0f, tracks));
            DrawGraphSplitter(y + tracks, splitter, available);
            DrawGraph(ImRect(ImVec2(window->WorkRect.Min.x, y + tracks + splitter), window->WorkRect.Max));
        }
        else
        {
            DrawTracks(ImVec2(0.0f, available));
        }
        ImGui::EndDisabled();
        DrawTransportPopups();
    }

    void TimelineFrame::DrawGraphSplitter(float top, float height, float available)
    {
        auto window = ImGui::GetCurrentWindow();
        ImGui::SetCursorScreenPos(ImVec2(window->WorkRect.Min.x, top));
        ImGui::InvisibleButton("##graphSplitter", ImVec2(window->WorkRect.GetWidth(), height));
        AddRow("graph.splitter");
        const bool active = ImGui::IsItemActive();
        if (active || ImGui::IsItemHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (active)
            graphHeight = std::clamp(window->WorkRect.Max.y - ImGui::GetMousePos().y - height / 2, 40.0f,
                                     std::max(available - rowHeight * 2, 40.0f));
        const auto middle = std::floor(top + height / 2);
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(window->WorkRect.Min.x, middle), ImVec2(window->WorkRect.Max.x, middle),
            ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive : ImGuiCol_Separator), active ? 2.0f : 1.0f);
    }

    void TimelineFrame::DrawGraph(const ImRect& rect)
    {
        TimelineGraph::Frame frame;
        frame.axis = axis;
        frame.headerX = rect.Min.x;
        frame.rect = rect;
        frame.endTick = endTick;
        // (what the drag will move is selected by then)
        if (timeline.IsSnapping())
            frame.getSnapTargets = [this] {
                CollectSnapTargets(axis, TimelineSelection::Get(), true);
                return snapTargets;
            };
        frame.snapTolerance = axis.TicksPer(SNAP_PIXELS);
        frame.setView = [this](TimelineMath::View view) { timeline.SetView(view); };
        frame.openItemMenu = [](std::uint32_t tick) {
            menuTick = tick;
            menuKind = MenuKind::Items;
            ImGui::OpenPopup(ITEM_MENU);
        };
        frame.drawMenus = [this] { DrawMenus(); };
        TimelineGraph::Draw(frame);
        if (TimelineGraph::IsOpen())
        {
            const auto plot = TimelineGraph::GetPlot();
            timeline.rows.push_back({"graph", plot.Min, plot.Max});
            for (const auto& [name, each] : TimelineGraph::GetLegend())
                timeline.rows.push_back({"graph." + name, each.Min, each.Max});
        }
    }

    void TimelineFrame::DrawTransport()
    {
        const auto& style = ImGui::GetStyle();
        const auto buttonSize = ImVec2(ImGui::GetFrameHeight() * 1.3f, 0);
        const auto shortcut = [](std::string_view id) {
            const auto keys = Commands::GetShortcut(id);
            return keys.empty() ? std::string() : " (" + keys + ")";
        };
        // a button that stays pressed while what it toggles is on
        const auto toggle = [](const char* label, bool on, std::optional<ImVec4> color = std::nullopt) {
            if (on)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, color.value_or(ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)));
                if (color)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
            }
            const bool pressed = ImGui::Button(label);
            if (on)
                ImGui::PopStyleColor(color ? 2 : 1);
            return pressed;
        };

        // (each button is a row too, for the harness)
        const auto row = [this](const char* name) {
            timeline.rows.push_back({name, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
        };

        const bool frozen = Components::Playback::IsGameFrozen();
        if (toggle(frozen ? ICON_FA_LOCK " Freeze time" : ICON_FA_LOCK_OPEN " Freeze time", frozen,
                   ImVec4(1.0f, 0.9f, 0.0f, 0.95f)))
            Commands::Run("playback.freeze");
        row("freeze");
        ImGui::SetItemTooltip(
            "%s", frozen ? std::format("Time is frozen at {}: the demo stands still while the "
                                       "timeline moves on its own{}",
                                       TimelineMath::FormatTime(*Components::Playback::GetFrozenTick()),
                                       shortcut("playback.freeze"))
                               .c_str()
                         : ("Freeze the demo here; the timeline goes on (for a frozen moment in a recording, add "
                            "bullet time from the ruler's right-click menu)" +
                            shortcut("playback.freeze"))
                               .c_str());

        ImGui::SameLine(0, style.ItemSpacing.x * 2);
        if (ImGui::Button(ICON_FA_BACKWARD_STEP "##frameBack", buttonSize))
            Commands::Run("playback.frameBack");
        ImGui::SetItemTooltip("%s", ("A frame back" + shortcut("playback.frameBack")).c_str());
        ImGui::SameLine();
        if (ImGui::Button(Components::Playback::IsPaused() ? ICON_FA_PLAY "##play" : ICON_FA_PAUSE "##play",
                          buttonSize))
            Commands::Run("playback.toggle");
        ImGui::SetItemTooltip(
            "%s", ((Components::Playback::IsPaused() ? "Play" : "Pause") + shortcut("playback.toggle")).c_str());
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FORWARD_STEP "##frameForward", buttonSize))
            Commands::Run("playback.frameForward");
        ImGui::SetItemTooltip("%s", ("A frame forward" + shortcut("playback.frameForward")).c_str());

        // the time, which a click types; as wide as the widest time of the demo (its digits differ in width), right
        // aligned, so neither the end's time nor what is right of it moves as it plays
        ImGui::SameLine(0, style.ItemSpacing.x * 2);
        const auto endTime = TimelineMath::FormatTime(endTick);
        const auto time = std::format("{} / {}##time", TimelineMath::FormatTime(currentTick), endTime);
        ImGui::PushFont(UIManager::Get().GetBoldFont());
        const auto timeWidth =
            std::max(ImGui::CalcTextSize(std::format("{} / {}", WithWidestDigits(endTime), endTime).c_str()).x,
                     ImGui::CalcTextSize(time.c_str(), nullptr, true).x) +
            style.FramePadding.x * 2;
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(1.0f, 0.5f));
        if (ImGui::Button(time.c_str(), ImVec2(timeWidth, 0)))
        {
            std::snprintf(timeText, sizeof(timeText), "%s", TimelineMath::FormatTime(currentTick).c_str());
            ImGui::OpenPopup(TIME_POPUP);
        }
        ImGui::PopStyleVar();
        row("time");
        ImGui::PopFont();
        ImGui::SetItemTooltip("Tick %u of %u; click to type a time to go to", currentTick, endTick);

        // the speed: a slider through Playback's steps, as IWXMVM had it
        ImGui::SameLine();
        if (timescale)
        {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            ImGuiEx::TimescaleSlider("##speed", &timescale->value->floating_point,
                                     Components::Playback::TIMESCALE_STEPS.front(),
                                     Components::Playback::TIMESCALE_STEPS.back(), "%.3f",
                                     ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoInput);
            row("speed");
            ImGui::SetItemTooltip(
                "%s", ("Playback speed" + shortcut("playback.slower") + shortcut("playback.faster")).c_str());
        }

        ImGui::SameLine(0, style.ItemSpacing.x * 2);
        if (toggle(ICON_FA_REPEAT " Loop range", timeline.looping))
            Commands::Run("timeline.loop");
        row("loop");
        ImGui::SetItemTooltip("Play the capture range over and over: back to its start at its end");
        ImGui::SameLine();
        if (toggle(ICON_FA_MAGNET " Snap", timeline.snapping))
            Commands::Run("timeline.snap");
        row("snap");
        ImGui::SetItemTooltip(
            "Snap what is dragged to the playhead, keys, clips, kills, the capture range, markers "
            "and whole seconds nearby; hold Alt to drag freely");

        // on the right: Fit, Range, and the keys' file menu
        const auto fitLabel = ICON_FA_EXPAND " Fit";
        const auto rangeLabel = ICON_FA_ARROWS_LEFT_RIGHT_TO_LINE " Range";
        const auto right = ImGui::CalcTextSize(fitLabel).x + ImGui::CalcTextSize(rangeLabel).x +
                           ImGui::CalcTextSize(ICON_FA_ELLIPSIS).x + style.FramePadding.x * 6 + style.ItemSpacing.x * 2;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - right));
        if (ImGui::Button(fitLabel))
            Commands::Run("timeline.fit");
        ImGui::SetItemTooltip("%s", ("Show the whole demo" + shortcut("timeline.fit")).c_str());
        ImGui::SameLine();
        if (ImGui::Button(rangeLabel))
            Commands::Run("timeline.range");
        ImGui::SetItemTooltip("%s", ("Show the capture range" + shortcut("timeline.range")).c_str());
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ELLIPSIS "##keysMenu"))
            ImGui::OpenPopup(KEYS_MENU);
        row("keys");
        ImGui::SetItemTooltip("Import, export or clear the keys");
    }

    void TimelineFrame::DrawTransportPopups()
    {
        if (ImGui::BeginPopup(TIME_POPUP))
        {
            ImGui::TextDisabled("Go to (0:41.20, 41.2 or 41200t)");
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            if (ImGui::InputText("##time", timeText, sizeof(timeText), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                if (const auto tick = TimelineMath::ParseTime(timeText))
                {
                    Seek(*tick, false);
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    Toasts::Show(std::format("\"{}\" isn't a time", timeText));
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup(KEYS_MENU))
        {
            const auto& all = KeyframeManager::Get().GetKeyframes();
            const bool hasKeys = std::ranges::any_of(all, [](const auto& pair) { return !pair.second.empty(); });
            if (ImGui::MenuItem(ICON_FA_FILE_IMPORT " Import keys..."))
                Commands::Run("file.importKeys");
            if (ImGui::MenuItem(ICON_FA_FILE_ARROW_DOWN " Export keys...", nullptr, false, hasKeys))
                Commands::Run("file.exportKeys");
            ImGui::Separator();
            bool clear = false;
            if (ImGui::MenuItem(ICON_FA_TRASH_CAN " Clear all keys...", nullptr, false, hasKeys))
                clear = true;
            ImGui::EndPopup();
            if (clear)
                ImGui::OpenPopup(CLEAR_POPUP);
        }

        if (ImGui::BeginPopupModal(CLEAR_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("This deletes every key and campath node (one undo brings them back)");
            if (ImGui::Button("Delete"))
            {
                KeyframeManager::Get().ClearKeyframes();
                Components::KeyframeSerializer::WriteRecentSoon();
                Toasts::Show("All keys cleared", true);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Keep"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void TimelineFrame::DrawRuler()
    {
        auto drawList = ImGui::GetWindowDrawList();
        const auto& style = ImGui::GetStyle();
        drawList->AddRectFilled(ruler.Min, ruler.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        drawList->PushClipRect(ruler.Min, ruler.Max, true);

        // which part of the demo shows, along the top, when it isn't all of it
        if (endTick > 0 && (axis.view.start > 0 || static_cast<std::uint32_t>(axis.view.end) < endTick))
        {
            const auto from = ruler.Min.x + ruler.GetWidth() * axis.view.start / endTick;
            const auto to = ruler.Min.x + ruler.GetWidth() * axis.view.end / endTick;
            drawList->AddRectFilled(ImVec2(from, ruler.Min.y), ImVec2(std::max(to, from + 2), ruler.Min.y + 3),
                                    ImGui::GetColorU32(ImGuiCol_ButtonHovered));
        }

        const auto lineColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const auto from = static_cast<std::uint32_t>(std::max(axis.view.start, 0));
        const auto to = static_cast<std::uint32_t>(std::max(axis.view.end, 0)) + 1;
        TimelineMath::ForEachLine(from, to, spacing.minor, [&](std::uint32_t tick) {
            const auto x = std::floor(axis.ToX(tick));
            drawList->AddLine(ImVec2(x, ruler.Max.y - ruler.GetHeight() * 0.25f), ImVec2(x, ruler.Max.y), lineColor);
        });
        const auto labelSize = ImGui::GetFontSize() * 0.8f;
        TimelineMath::ForEachLine(from, to, spacing.major, [&](std::uint32_t tick) {
            const auto x = std::floor(axis.ToX(tick));
            drawList->AddLine(ImVec2(x, ruler.Min.y + 3), ImVec2(x, ruler.Max.y), lineColor);
            drawList->AddText(ImGui::GetFont(), labelSize, ImVec2(x + 3, ruler.Min.y + 2),
                              ImGui::GetColorU32(ImGuiCol_Text),
                              TimelineMath::FormatRulerTime(tick, spacing.major).c_str());
        });

        // the playhead's handle
        const auto playheadX = std::floor(axis.ToX(currentTick));
        const auto handle = ImGui::GetFontSize() * 0.4f;
        drawList->AddTriangleFilled(ImVec2(playheadX - handle, ruler.Min.y), ImVec2(playheadX + handle, ruler.Min.y),
                                    ImVec2(playheadX, ruler.Min.y + handle * 1.5f), Theme::Playhead());
        drawList->AddLine(ImVec2(playheadX, ruler.Min.y), ImVec2(playheadX, ruler.Max.y), Theme::Playhead(), 2.0f);
        if (snappedTick &&
            (drag.gesture == Gesture::Move || drag.gesture == Gesture::Scrub || drag.gesture == Gesture::Trim ||
             drag.gesture == Gesture::Range || drag.gesture == Gesture::SpanEnd || drag.gesture == Gesture::SlidePath))
        {
            const auto x = std::floor(axis.ToX(static_cast<double>(*snappedTick)));
            drawList->AddLine(ImVec2(x, ruler.Min.y), ImVec2(x, ruler.Max.y), SNAP_COLOR);
        }
        drawList->PopClipRect();
    }

    void TimelineFrame::DrawMarkersRow()
    {
        using namespace Components;
        auto drawList = ImGui::GetWindowDrawList();
        const auto& style = ImGui::GetStyle();
        const auto& row = markersRow;
        drawList->AddRectFilled(row.Min, row.Max, ImGui::GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);

        // its name on the left
        drawList->AddText(ImVec2(headerX + 4, row.Min.y + (row.GetHeight() - ImGui::GetTextLineHeight()) / 2),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_FLAG " Markers");
        drawList->PushClipRect(row.Min, row.Max, true);

        // the capture range: a band, with handles at its ends
        const auto [captureStart, captureEnd] = GetCaptureRange();
        const auto bandStart = std::floor(axis.ToX(captureStart));
        const auto bandEnd = std::floor(axis.ToX(captureEnd));
        const auto handleWidth = 4.0f;
        rangeHandles.band = ImRect(ImVec2(std::min(bandStart, bandEnd), row.Min.y + 2),
                                   ImVec2(std::max(bandStart, bandEnd), row.Max.y - 2));
        rangeHandles.start =
            ImRect(ImVec2(bandStart - handleWidth, row.Min.y), ImVec2(bandStart + handleWidth, row.Max.y));
        rangeHandles.end = ImRect(ImVec2(bandEnd - handleWidth, row.Min.y), ImVec2(bandEnd + handleWidth, row.Max.y));
        drawList->AddRectFilled(rangeHandles.band.Min, rangeHandles.band.Max, WithAlpha(CAPTURE_COLOR, 45));
        drawList->AddRect(rangeHandles.band.Min, rangeHandles.band.Max, WithAlpha(CAPTURE_COLOR, 140));
        for (const auto x : {bandStart, bandEnd})
            drawList->AddRectFilled(ImVec2(x - 1.5f, row.Min.y), ImVec2(x + 1.5f, row.Max.y), CAPTURE_COLOR);

        // bomb plants across it, kills from the bottom (the player's own taller)
        const auto mouse = ImGui::GetMousePos();
        const bool tooltips = markersHovered && drag.gesture == Gesture::None && !ImGui::IsAnyMouseDown();
        std::vector<std::string> tooltip;

        // bullet time: a band where the demo stands still, with how long, and a handle at its end
        spanHandles.clear();
        {
            const auto spans = Playback::GetFrozenSpans();
            for (std::size_t i = 0; i < spans->size(); ++i)
            {
                const auto start = FrozenSpans::GetStart(*spans, i);
                const auto x0 = std::floor(axis.ToX(start));
                const auto x1 = std::floor(axis.ToX(start + (*spans)[i].length));
                const ImRect band(ImVec2(x0, row.Min.y + 1), ImVec2(std::max(x1, x0 + 2), row.Max.y - 1));
                const ImRect end(ImVec2(band.Max.x - 4, row.Min.y), ImVec2(band.Max.x + 4, row.Max.y));
                spanHandles.push_back({band, end, i});
                if (band.Max.x < row.Min.x || band.Min.x > row.Max.x)
                    continue;
                drawList->AddRectFilled(band.Min, band.Max, WithAlpha(SPAN_COLOR, 70));
                drawList->AddRect(band.Min, band.Max, WithAlpha(SPAN_COLOR, 200));
                drawList->AddRectFilled(ImVec2(band.Max.x - 1.5f, row.Min.y), ImVec2(band.Max.x + 1.5f, row.Max.y),
                                        SPAN_COLOR);
                const auto label = std::format(ICON_FA_SNOWFLAKE " {:.1f} s{}", (*spans)[i].length / 1000.0,
                                               (*spans)[i].actorsMove ? ", actors move" : "");
                drawList->PushClipRect(band.Min, band.Max, true);
                drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f, ImVec2(band.Min.x + 3, row.Min.y + 2),
                                  IM_COL32_WHITE, label.c_str());
                drawList->PopClipRect();
                if (tooltips && band.Contains(mouse))
                    tooltip.push_back(
                        std::format("{}\n      The demo stands still there while the camera goes on; drag "
                                    "its end to change how long, right-click for more",
                                    BulletTime::Describe(i)));
            }
        }
        const auto inView = [&](std::uint32_t tick) {
            return static_cast<std::int64_t>(tick) >= axis.view.start &&
                   static_cast<std::int64_t>(tick) <= axis.view.end;
        };
        if (const auto events = KillFinder::GetEvents())
        {
            for (const auto& moment : events->moments)
            {
                const auto at = Playback::ToTimelineTick(moment.tick);
                if (!inView(at))
                    continue;
                const auto x = std::floor(axis.ToX(at));
                drawList->AddRectFilled(ImVec2(x - 1, row.Min.y), ImVec2(x + 1, row.Max.y), BOMB_COLOR);
                if (tooltips && std::abs(mouse.x - x) <= 3.0f)
                    tooltip.push_back(std::format("{}  Bomb planted", KillFinder::FormatTick(at)));
            }
        }
        for (const auto& entry : KillFinder::GetEntries())
        {
            const auto tick = Playback::ToTimelineTick(KillFinder::GetKill(entry).tick);
            if (!KillFinder::IsShown(entry) || !inView(tick))
                continue;
            const auto x = std::floor(axis.ToX(tick));
            const bool own = entry.byPov || entry.ofPov;
            const auto top = row.Min.y + row.GetHeight() * (own ? 0.15f : 0.55f);
            drawList->AddRectFilled(ImVec2(x - 1, top), ImVec2(x + 1, row.Max.y),
                                    ImGui::GetColorU32(KillFinder::GetColor(entry)));
            if (tooltips && std::abs(mouse.x - x) <= 3.0f)
                tooltip.push_back(std::format("{}  {}\n      {}", KillFinder::FormatTick(tick),
                                              KillFinder::Describe(entry), KillFinder::GetDetails(entry)));
        }

        // the frozen tick
        if (const auto frozen = Playback::GetFrozenTick(); frozen && inView(*frozen))
        {
            const auto x = std::floor(axis.ToX(*frozen));
            drawList->AddRectFilled(ImVec2(x - 1, row.Min.y), ImVec2(x + 1, row.Max.y), FROZEN_COLOR);
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.7f, ImVec2(x + 3, row.Min.y + 2), FROZEN_COLOR,
                              ICON_FA_LOCK);
            if (tooltips && std::abs(mouse.x - x) <= 3.0f)
                tooltip.push_back(std::format("{}  Time frozen here", TimelineMath::FormatTime(*frozen)));
        }

        // the user's markers: flags, with their names where there is room
        const auto& markers = Markers::GetAll();
        for (std::size_t i = 0; i < markers.size(); ++i)
        {
            const auto& marker = markers[i];
            if (!inView(marker.tick))
                continue;
            const auto x = std::floor(axis.ToX(marker.tick));
            const Item item{Kind::Marker, 0, marker.id};
            const bool selected = TimelineSelection::IsSelected(item);
            const ImRect rect(ImVec2(x - 5, row.Min.y), ImVec2(x + 5, row.Max.y));
            const bool hovered = markersHovered && drag.gesture == Gesture::None && rect.Contains(mouse);
            const auto color = selected  ? ImGui::GetColorU32(ImGuiCol_CheckMark)
                               : hovered ? IM_COL32_WHITE
                                         : MARKER_COLOR;
            const auto flagSize = row.GetHeight() * 0.45f;
            drawList->AddLine(ImVec2(x, row.Min.y + 1), ImVec2(x, row.Max.y), color, 2.0f);
            drawList->AddTriangleFilled(ImVec2(x, row.Min.y + 1), ImVec2(x + flagSize, row.Min.y + 1 + flagSize / 2),
                                        ImVec2(x, row.Min.y + 1 + flagSize), color);
            const auto next = i + 1 < markers.size() ? axis.ToX(markers[i + 1].tick) : row.Max.x;
            const auto nameSize = ImGui::CalcTextSize(marker.name.c_str()).x * 0.8f;
            if (next - x > nameSize + flagSize + 8)
                drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.8f,
                                  ImVec2(x + flagSize + 3, row.Min.y + 2), color, marker.name.c_str());
            drawn.push_back({{item}, rect, marker.tick, marker.tick, MARKERS_ROW});
        }

        if (snappedTick &&
            (drag.gesture == Gesture::Move || drag.gesture == Gesture::Scrub || drag.gesture == Gesture::Trim ||
             drag.gesture == Gesture::Range || drag.gesture == Gesture::SpanEnd || drag.gesture == Gesture::SlidePath))
        {
            const auto x = std::floor(axis.ToX(static_cast<double>(*snappedTick)));
            drawList->AddLine(ImVec2(x, row.Min.y), ImVec2(x, row.Max.y), SNAP_COLOR);
        }
        const auto playheadX = std::floor(axis.ToX(currentTick));
        drawList->AddLine(ImVec2(playheadX, row.Min.y), ImVec2(playheadX, row.Max.y), Theme::Playhead(), 2.0f);
        drawList->PopClipRect();

        if (!tooltip.empty() && ImGui::BeginTooltip())
        {
            for (const auto& line : tooltip)
                ImGui::TextUnformatted(line.c_str());
            ImGui::EndTooltip();
        }
    }

    ImRect TimelineFrame::AddLane(RowKind kind, std::uint32_t owner, float top, float height, std::string name,
                                  bool background)
    {
        const ImRect rect(ImVec2(axis.x0, top), ImVec2(axis.x0 + axis.width, top + height));
        lanes.push_back({kind, owner, rect});
        timeline.rows.push_back({std::move(name), rect.Min, rect.Max});
        if (background)
        {
            auto drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(rect.Min, rect.Max, ImGui::GetColorU32(ImGuiCol_FrameBg),
                                    ImGui::GetStyle().FrameRounding);
            DrawLines(rect);
        }
        return rect;
    }

    // the ruler's labelled lines, faintly across a lane
    void TimelineFrame::DrawLines(const ImRect& rect)
    {
        auto drawList = ImGui::GetWindowDrawList();
        const auto color = IM_COL32(255, 255, 255, 14);
        TimelineMath::ForEachLine(static_cast<std::uint32_t>(std::max(axis.view.start, 0)),
                                  static_cast<std::uint32_t>(std::max(axis.view.end, 0)) + 1, spacing.major,
                                  [&](std::uint32_t tick) {
                                      const auto x = std::floor(axis.ToX(tick));
                                      drawList->AddLine(ImVec2(x, rect.Min.y), ImVec2(x, rect.Max.y), color);
                                  });
    }

    void TimelineFrame::DrawKeys(const Types::KeyframeableProperty& property, const std::vector<Types::Keyframe>& keys,
                                 const ImRect& rect, std::size_t lane)
    {
        auto drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(rect.Min, rect.Max, true);
        const bool campath = property.type == Types::KeyframeablePropertyType::CampathCamera;
        const auto radius = std::floor(rect.GetHeight() * 0.3f);
        const auto middle = std::floor((rect.Min.y + rect.Max.y) / 2);
        const auto mouse = ImGui::GetMousePos();

        // the campath: a thin line from node to node, with how fast the camera goes along it where there is room; at
        // constant speed one speed, and small diamonds where the camera passes the nodes
        if (campath)
        {
            auto& keyframeManager = KeyframeManager::Get();
            const bool constant = keyframeManager.IsConstantSpeed() && keys.size() >= 2;
            const auto duration = keys.empty() ? 0.0f : (keys.back().tick - keys.front().tick) / 1000.0f;
            const auto constantSpeed =
                constant && duration > 0 ? static_cast<float>(keyframeManager.GetCampathLength()) / duration : 0.0f;
            for (std::size_t i = 0; i + 1 < keys.size(); ++i)
            {
                const auto x0 = axis.ToX(keys[i].tick), x1 = axis.ToX(keys[i + 1].tick);
                if (x1 < rect.Min.x || x0 > rect.Max.x)
                    continue;
                drawList->AddLine(ImVec2(x0, middle), ImVec2(x1, middle), IM_COL32(150, 150, 150, 120));
                const auto seconds = (keys[i + 1].tick - keys[i].tick) / 1000.0f;
                const auto distance =
                    glm::distance(keys[i].value.cameraData.position, keys[i + 1].value.cameraData.position);
                const auto label = constant ? std::format("{:.0f} u/s", constantSpeed)
                                            : std::format("{:.0f} u/s", seconds > 0 ? distance / seconds : 0.0f);
                const auto labelSize = ImGui::CalcTextSize(label.c_str()) * 0.7f;
                if (x1 - x0 > labelSize.x + radius * 4)
                    drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.7f,
                                      ImVec2((x0 + x1 - labelSize.x) / 2, middle - labelSize.y - 1),
                                      IM_COL32(170, 170, 170, 200), label.c_str());
            }
            for (std::size_t i = 1; constant && i + 1 < keys.size(); ++i)
            {
                const auto x = std::floor(axis.ToX(keyframeManager.GetCampathPassingTick(keys[i].tick)));
                const auto size = std::max(std::floor(radius * 0.55f), 2.0f);
                drawList->AddQuad(ImVec2(x, middle - size), ImVec2(x + size, middle), ImVec2(x, middle + size),
                                  ImVec2(x - size, middle), IM_COL32(200, 200, 200, 170), 1.0f);
            }
        }

        for (const auto& key : keys)
        {
            const auto x = std::floor(axis.ToX(key.tick));
            if (x < rect.Min.x - radius || x > rect.Max.x + radius)
                continue;
            const auto item = TimelineSelection::KeyItem(property.type, key.id);
            const ImRect hit(ImVec2(x - radius - 2, rect.Min.y), ImVec2(x + radius + 2, rect.Max.y));
            const bool selected = TimelineSelection::IsSelected(item);
            const bool hovered = lanesHovered && drag.gesture == Gesture::None && hit.Contains(mouse);
            DrawDiamond(drawList, ImVec2(x, middle), radius, GetKeyFill(selected, hovered),
                        selected ? std::optional(IM_COL32_WHITE) : std::nullopt);
            drawn.push_back({{item}, hit, key.tick, key.tick, lane});
        }
        drawList->PopClipRect();
    }

    void TimelineFrame::DrawHeaderName(float y, float height, float indent, const char* text,
                                       std::optional<float> right)
    {
        auto drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(ImVec2(headerX, y), ImVec2(right.value_or(axis.x0 - 4), y + height), true);
        drawList->AddText(ImVec2(headerX + indent, y + (height - ImGui::GetTextLineHeight()) / 2),
                          ImGui::GetColorU32(ImGuiCol_Text), text);
        drawList->PopClipRect();
    }

    // a small button at the right of a track's header
    bool TimelineFrame::DrawHeaderButton(float y, float height, const char* label, const char* tooltip)
    {
        const auto size = ImVec2(ImGui::GetFontSize() * 1.4f, std::min(height, ImGui::GetFrameHeight()));
        ImGui::SetCursorScreenPos(ImVec2(axis.x0 - size.x - 6, y + (height - size.y) / 2));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 0));
        const bool pressed = ImGui::Button(label, size);
        ImGui::PopStyleVar();
        ImGui::SetItemTooltip("%s", tooltip);
        return pressed;
    }

    float TimelineFrame::DrawKeyTrack(const Types::KeyframeableProperty& property, float y, float indent,
                                      std::optional<std::string_view> label)
    {
        auto& keyframeManager = KeyframeManager::Get();
        auto& keys = keyframeManager.GetKeyframes(property);
        const bool campath = property.type == Types::KeyframeablePropertyType::CampathCamera;
        const auto rowName = campath ? std::string("campath") : KeyframeManager::GetTypeName(property.type);
        ImGui::PushID(std::format("property{}", static_cast<int>(property.type)).c_str());

        // the header: a button putting its values in the graph, its name, and one clearing it
        const bool inGraph = TimelineGraph::Shows(property.type);
        const auto buttonSize = ImVec2(std::floor(ImGui::GetFontSize() * 1.3f), ImGui::GetFontSize());
        ImGui::SetCursorScreenPos(ImVec2(headerX + indent, y + (rowHeight - buttonSize.y) / 2));
        ImGui::PushStyleColor(ImGuiCol_Button,
                              inGraph ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 0));
        if (ImGui::Button(ICON_FA_CHART_LINE "##graph", buttonSize))
            TimelineGraph::Toggle(property.type);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        AddRow("graph." + rowName);
        ImGui::SetItemTooltip(inGraph ? "Take its values out of the graph"
                                      : "Show its values in the graph under the tracks, to change them");
        const auto name = campath ? std::format(ICON_FA_VIDEO " Campath ({})", keys.size())
                                  : std::string(label.value_or(property.name));
        DrawHeaderName(y, rowHeight, indent + buttonSize.x + 4, name.c_str(),
                       axis.x0 - ImGui::GetFontSize() * 1.4f - 10);
        if (DrawHeaderButton(y, rowHeight, ICON_FA_TRASH "##clear", campath ? "Clear the campath" : "Clear these keys"))
        {
            if (campath)
            {
                Commands::Run("campath.clear");
            }
            else
            {
                keyframeManager.ClearKeyframes(property);
                keyframeManager.SortAndSaveKeyframes(keys);
                shownProperties.erase(property.type);
                Toasts::Show("Keys cleared", true);
            }
        }

        const auto lane = lanes.size();
        const auto rect = AddLane(campath ? RowKind::Campath : RowKind::Property,
                                  static_cast<std::uint32_t>(property.type), y, rowHeight, rowName);
        DrawKeys(property, keys, rect, lane);
        if (campath && keys.empty())
        {
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(rect.Min.x + 8, rect.Min.y + (rect.GetHeight() - ImGui::GetTextLineHeight()) / 2),
                ImGui::GetColorU32(ImGuiCol_TextDisabled),
                std::format("No campath yet: fly ({}, or hold the right mouse button) and press {} to add a node",
                            Commands::GetShortcut("fly.toggle"), Commands::GetShortcut("campath.addNode"))
                    .c_str());
        }
        y += rowHeight + ImGui::GetStyle().ItemSpacing.y;
        ImGui::PopID();
        return y;
    }

    float TimelineFrame::DrawTrackGroup(float y, bool boneCamera)
    {
        using Components::BoneCameraLogic::KEYED_PROPERTIES;
        std::vector<const Types::KeyframeableProperty*> shown;
        for (const auto& [property, keys] : KeyframeManager::Get().GetKeyframes())
        {
            // (a light's are on its own track)
            if (property.type != Types::KeyframeablePropertyType::CampathCamera &&
                !Components::RuntimeProperties::IsActorProperty(property.type) &&
                Components::BoneCameraLogic::IsKeyedProperty(property.type) == boneCamera && IsShown(property, keys))
                shown.push_back(&property);
        }
        if (shown.empty())
            return y;
        // (the bone camera's in the Camera tab's order)
        if (boneCamera)
            std::ranges::sort(shown, {}, [](const Types::KeyframeableProperty* property) {
                return std::ranges::find(KEYED_PROPERTIES, property->type) - KEYED_PROPERTIES.begin();
            });
        auto& open = boneCamera ? boneCameraOpen : lookOpen;
        const std::string rowName = boneCamera ? "bonecam" : "look";
        ImGui::PushID(rowName.c_str());

        // the group's header, with every key of its tracks
        const auto arrowSize = ImGui::GetFontSize();
        ImGui::SetCursorScreenPos(ImVec2(headerX, y + (rowHeight - arrowSize) / 2));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::Button(open ? ICON_FA_CARET_DOWN "##open" : ICON_FA_CARET_RIGHT "##open",
                          ImVec2(arrowSize, arrowSize)))
            open = !open;
        ImGui::PopStyleColor();

        // a graph button for the values of all its tracks with keys, as each track's is for its own
        std::vector<Types::KeyframeablePropertyType> keyed;
        std::vector<bool> inGraph;
        for (const auto property : shown)
        {
            if (KeyframeManager::Get().GetKeyframes(*property).empty())
                continue;
            keyed.push_back(property->type);
            inGraph.push_back(TimelineGraph::Shows(property->type));
        }
        const bool allInGraph = !keyed.empty() && std::ranges::all_of(inGraph, std::identity{});
        const auto buttonSize = ImVec2(std::floor(ImGui::GetFontSize() * 1.3f), ImGui::GetFontSize());
        ImGui::SetCursorScreenPos(ImVec2(headerX + arrowSize + 2, y + (rowHeight - buttonSize.y) / 2));
        ImGui::BeginDisabled(keyed.empty());
        ImGui::PushStyleColor(ImGuiCol_Button,
                              allInGraph ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 0));
        if (ImGui::Button(ICON_FA_CHART_LINE "##graph", buttonSize))
        {
            for (const auto index : TimelineMath::GetGroupToggle(inGraph))
                TimelineGraph::Toggle(keyed[index]);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        AddRow("graph." + rowName);
        if (boneCamera)
            ImGui::SetItemTooltip(allInGraph ? "Take the bone camera's values out of the graph"
                                             : "Show the values of every bone camera track with keys in the graph");
        else
            ImGui::SetItemTooltip(allInGraph ? "Take the Look's values out of the graph"
                                             : "Show the values of every Look track with keys in the graph under the "
                                               "tracks, to change them");
        DrawHeaderName(y, rowHeight, arrowSize + 2 + buttonSize.x + 4,
                       boneCamera ? ICON_FA_BONE " Bone camera" : ICON_FA_SUN " Look");
        ImGui::PopID();

        const auto lane = lanes.size();
        const auto rect = AddLane(RowKind::Look, boneCamera ? 1 : 0, y, rowHeight, rowName);
        std::map<std::uint32_t, std::vector<Item>> byTick;
        for (const auto property : shown)
        {
            for (const auto& key : KeyframeManager::Get().GetKeyframes(*property))
                byTick[key.tick].push_back(TimelineSelection::KeyItem(property->type, key.id));
        }
        auto drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(rect.Min, rect.Max, true);
        const auto radius = std::floor(rect.GetHeight() * 0.22f);
        const auto mouse = ImGui::GetMousePos();
        for (const auto& [tick, items] : byTick)
        {
            const auto x = std::floor(axis.ToX(tick));
            if (x < rect.Min.x - radius || x > rect.Max.x + radius)
                continue;
            const ImRect hit(ImVec2(x - radius - 2, rect.Min.y), ImVec2(x + radius + 2, rect.Max.y));
            const bool selected = std::ranges::all_of(items, TimelineSelection::IsSelected);
            const bool hovered = lanesHovered && drag.gesture == Gesture::None && hit.Contains(mouse);
            DrawDiamond(drawList, ImVec2(x, (rect.Min.y + rect.Max.y) / 2), radius, GetKeyFill(selected, hovered),
                        std::nullopt);
            drawn.push_back({items, hit, tick, tick, lane});
        }
        drawList->PopClipRect();
        y += rowHeight + ImGui::GetStyle().ItemSpacing.y;

        if (open)
        {
            for (const auto property : shown)
                y = DrawKeyTrack(*property, y, arrowSize);
        }
        return y;
    }

    // An effect in the scene's track: one row with its bar from its start to its end, an
    // item of the selection, moved by its middle and trimmed by its ends (Gesture::EffectBar) as clips are; what of it
    // plays filled, the rest
    // of the bar (one that plays once and is over) faint; a mark where it plays again, and a Fade's tail past its end
    float TimelineFrame::DrawEffectTrack(const Types::Actor& actor, float y)
    {
        using namespace ImGui;
        auto drawList = GetWindowDrawList();
        const auto& style = GetStyle();
        const auto& effect = *actor.effect;
        const auto info = Components::Actors::GetEffectInfo(effect.name).value_or(Types::ActorCatalog::EffectInfo{});
        PushID(std::format("effect{}", actor.id).c_str());

        SetCursorScreenPos(ImVec2(headerX, y));
        const auto headerWidth = axis.x0 - headerX - 6;
        const bool selected = Components::Actors::GetSelected() == actor.id;
        if (Selectable(std::format(ICON_FA_WAND_MAGIC_SPARKLES " {}##effect", actor.name).c_str(), selected, 0,
                       ImVec2(headerWidth, rowHeight)))
        {
            Components::Actors::Select(actor.id);
            UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
        }
        SetItemTooltip("Drag its bar to move it, or its ends to trim it");
        AddRowName(actor);

        const auto lane = lanes.size();
        const auto row = AddLane(RowKind::Effect, actor.id, y, rowHeight, std::format("actor {} effect", actor.id));
        drawList->PushClipRect(row.Min, row.Max, true);
        const auto x0 = std::floor(axis.ToX(ActorToTimeline(effect.start)));
        const auto x1 = std::floor(std::max(axis.ToX(ActorToTimeline(effect.end)), x0 + 4));
        const auto played =
            std::clamp(std::floor(axis.ToX(ActorToTimeline(ActorLogic::GetEffectPlayedEnd(effect, info)))), x0, x1);
        const auto mouse = GetMousePos();
        const ImRect bar(ImVec2(x0, row.Min.y + 2), ImVec2(x1, row.Max.y - 2));
        const bool hovered = lanesHovered && drag.gesture == Gesture::None && bar.Contains(mouse);
        const auto item = TimelineSelection::BarItem(actor);
        const bool barSelected = selected || TimelineSelection::IsSelected(item);
        // (the color of effects: warm, as the mockup has it)
        const auto fill = hovered || barSelected ? IM_COL32(160, 106, 52, 255) : IM_COL32(138, 90, 44, 255);
        drawList->AddRectFilled(bar.Min, ImVec2(played, bar.Max.y), fill, style.FrameRounding);
        if (played < x1)
            drawList->AddRectFilled(ImVec2(played, bar.Min.y + 4), ImVec2(x1, bar.Max.y - 4),
                                    IM_COL32(138, 90, 44, 90), style.FrameRounding);
        // where it plays again, and the tail of a Fade, as its particles finish
        const auto plays = ActorLogic::GetEffectPlays(actor, info);
        for (std::size_t i = 1; i < plays.size(); ++i)
        {
            const auto x = std::floor(axis.ToX(ActorToTimeline(plays[i].begin)));
            drawList->AddLine(ImVec2(x, bar.Min.y + 2), ImVec2(x, bar.Max.y - 2), IM_COL32(0, 0, 0, 110));
        }
        if (effect.ends == Types::EffectEnds::Fade && !plays.empty() && plays.back().kept)
            drawList->AddTriangleFilled(ImVec2(played, bar.Min.y), ImVec2(played + GetFontSize(), bar.GetCenter().y),
                                        ImVec2(played, bar.Max.y), IM_COL32(138, 90, 44, 110));
        if (barSelected)
        {
            drawList->AddRect(bar.Min, bar.Max, IM_COL32(208, 138, 60, 255), style.FrameRounding, 0, 1.5f);
            drawList->AddRectFilled(bar.Min, ImVec2(bar.Min.x + 3, bar.Max.y), IM_COL32(208, 138, 60, 255));
            drawList->AddRectFilled(ImVec2(bar.Max.x - 3, bar.Min.y), bar.Max, IM_COL32(208, 138, 60, 255));
        }
        drawList->PushClipRect(bar.Min, ImVec2(std::max(played, x0 + GetFontSize()), bar.Max.y), true);
        const auto shown = effect.name.substr(effect.name.find_last_of('/') + 1);
        drawList->AddText(ImVec2(bar.Min.x + 6, row.Min.y + style.FramePadding.y), GetColorU32(ImGuiCol_Text),
                          std::format(ICON_FA_WAND_MAGIC_SPARKLES " {}{}", shown,
                                      ActorLogic::PlaysLooping(effect, info) ? "" : " · once")
                              .c_str());
        drawList->PopClipRect();
        drawList->PopClipRect();
        if (bar.Max.x >= row.Min.x && bar.Min.x <= row.Max.x)
            drawn.push_back({{item}, bar, ActorToTimeline(effect.start), ActorToTimeline(effect.end), lane});

        // its ends are grabbed a few pixels in and out
        const auto edge = EDGE_PIXELS;
        effectHandles.push_back({actor.id, bar, ImRect(ImVec2(x0 - edge, bar.Min.y), ImVec2(x0 + edge, bar.Max.y)),
                                 ImRect(ImVec2(x1 - edge, bar.Min.y), ImVec2(x1 + edge, bar.Max.y))});
        if (lanesHovered && drag.gesture == Gesture::None)
        {
            const auto& handle = effectHandles.back();
            if (handle.start.Contains(mouse) || handle.end.Contains(mouse))
                SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (handle.bar.Contains(mouse) && !IsAnyMouseDown())
                SetTooltip("%s: %s to %s%s", actor.name.c_str(),
                           TimelineMath::FormatTime(ActorToTimeline(effect.start)).c_str(),
                           TimelineMath::FormatTime(ActorToTimeline(effect.end)).c_str(),
                           ActorLogic::PlaysLooping(effect, info) ? ", looping" : ", once");
        }
        PopID();
        return y + rowHeight + style.ItemSpacing.y;
    }

    // A light's track: one row with its bar, an item of the selection, moved and
    // trimmed as an effect's, and every key of its values on it; an arrow folds out a row for each value, with its graph
    float TimelineFrame::DrawLightTrack(const Types::Actor& actor, float y)
    {
        using namespace ImGui;
        namespace LightLogic = Components::LightLogic;
        auto drawList = GetWindowDrawList();
        const auto& style = GetStyle();
        const auto& light = *actor.light;
        const auto selected = Components::Actors::GetSelected() == actor.id;
        const auto open = openLights.contains(actor.id);
        PushID(std::format("light{}", actor.id).c_str());

        const auto arrowSize = GetFontSize();
        SetCursorScreenPos(ImVec2(headerX, y + (rowHeight - arrowSize) / 2));
        PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        if (Button(open ? ICON_FA_CARET_DOWN "##open" : ICON_FA_CARET_RIGHT "##open", ImVec2(arrowSize, arrowSize)))
        {
            if (open)
                openLights.erase(actor.id);
            else
                openLights.insert(actor.id);
        }
        PopStyleVar();
        PopStyleColor();
        AddRow(std::format("actor {} lightfold", actor.id));
        SetItemTooltip(open ? "Hide its values" : "Show a row for each of its values");

        SetCursorScreenPos(ImVec2(headerX + arrowSize + 2, y));
        if (Selectable(std::format(ICON_FA_LIGHTBULB " {}##light", actor.name).c_str(), selected, 0,
                       ImVec2(axis.x0 - headerX - arrowSize - 8, rowHeight)))
        {
            Components::Actors::Select(actor.id);
            UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
        }
        SetItemTooltip("Drag its bar to move it, or its ends to trim it");
        AddRowName(actor);

        const auto lane = lanes.size();
        const auto row = AddLane(RowKind::Light, actor.id, y, rowHeight, std::format("actor {} light", actor.id));
        drawList->PushClipRect(row.Min, row.Max, true);
        const auto x0 = std::floor(axis.ToX(ActorToTimeline(light.start)));
        const auto x1 = std::floor(std::max(axis.ToX(ActorToTimeline(light.end)), x0 + 4));
        const auto mouse = GetMousePos();
        const ImRect bar(ImVec2(x0, row.Min.y + 2), ImVec2(x1, row.Max.y - 2));
        const bool hovered = lanesHovered && drag.gesture == Gesture::None && bar.Contains(mouse);
        const auto item = TimelineSelection::BarItem(actor);
        const bool barSelected = selected || TimelineSelection::IsSelected(item);
        // (the color of lights: a dim gold, as the mockup has it)
        drawList->AddRectFilled(bar.Min, bar.Max,
                                hovered || barSelected ? IM_COL32(128, 108, 54, 255) : IM_COL32(106, 90, 44, 255),
                                style.FrameRounding);
        if (barSelected)
        {
            drawList->AddRect(bar.Min, bar.Max, IM_COL32(190, 165, 90, 255), style.FrameRounding, 0, 1.5f);
            drawList->AddRectFilled(bar.Min, ImVec2(bar.Min.x + 3, bar.Max.y), IM_COL32(190, 165, 90, 255));
            drawList->AddRectFilled(ImVec2(bar.Max.x - 3, bar.Min.y), bar.Max, IM_COL32(190, 165, 90, 255));
        }
        drawList->PushClipRect(bar.Min, bar.Max, true);
        drawList->AddText(ImVec2(bar.Min.x + 6, row.Min.y + style.FramePadding.y), GetColorU32(ImGuiCol_Text),
                          std::format(ICON_FA_LIGHTBULB " {}", actor.name).c_str());
        drawList->PopClipRect();
        // (before its keys, which are found first under the mouse)
        if (bar.Max.x >= row.Min.x && bar.Min.x <= row.Max.x)
            drawn.push_back({{item}, bar, ActorToTimeline(light.start), ActorToTimeline(light.end), lane});

        // every key of its values
        std::map<std::uint32_t, std::vector<Item>> byTick;
        for (const auto channel : LightLogic::ALL_CHANNELS)
        {
            const auto type = Components::RuntimeProperties::FindById(LightLogic::GetPropertyId(light.id, channel));
            if (!type || !KeyframeManager::Get().HasProperty(*type))
                continue;
            for (const auto& key : KeyframeManager::Get().GetKeyframes(KeyframeManager::Get().GetProperty(*type)))
                byTick[key.tick].push_back(TimelineSelection::KeyItem(*type, key.id));
        }
        const auto radius = std::floor(row.GetHeight() * 0.22f);
        for (const auto& [tick, items] : byTick)
        {
            const auto x = std::floor(axis.ToX(tick));
            if (x < row.Min.x - radius || x > row.Max.x + radius)
                continue;
            const ImRect hit(ImVec2(x - radius - 2, row.Min.y), ImVec2(x + radius + 2, row.Max.y));
            const bool keySelected = std::ranges::all_of(items, TimelineSelection::IsSelected);
            const bool keyHovered = lanesHovered && drag.gesture == Gesture::None && hit.Contains(mouse);
            DrawDiamond(drawList, ImVec2(x, (row.Min.y + row.Max.y) / 2), radius, GetKeyFill(keySelected, keyHovered),
                        std::nullopt);
            drawn.push_back({items, hit, tick, tick, lane});
        }
        drawList->PopClipRect();

        // its ends are grabbed a few pixels in and out
        const auto edge = EDGE_PIXELS;
        effectHandles.push_back({actor.id, bar, ImRect(ImVec2(x0 - edge, bar.Min.y), ImVec2(x0 + edge, bar.Max.y)),
                                 ImRect(ImVec2(x1 - edge, bar.Min.y), ImVec2(x1 + edge, bar.Max.y))});
        if (lanesHovered && drag.gesture == Gesture::None)
        {
            const auto& handle = effectHandles.back();
            if (handle.start.Contains(mouse) || handle.end.Contains(mouse))
                SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (handle.bar.Contains(mouse) && !IsAnyMouseDown())
                SetTooltip("%s: %s to %s", actor.name.c_str(),
                           TimelineMath::FormatTime(ActorToTimeline(light.start)).c_str(),
                           TimelineMath::FormatTime(ActorToTimeline(light.end)).c_str());
        }
        PopID();
        y += rowHeight + style.ItemSpacing.y;

        if (open)
        {
            // (its place's rows too, : on a bone its Position is its Offset)
            for (const auto channel : LightLogic::ALL_CHANNELS)
            {
                if ((channel == LightLogic::Channel::Cone || channel == LightLogic::Channel::Aim) &&
                    light.kind != Types::LightKind::Spot)
                    continue;
                const bool offset = channel == LightLogic::Channel::Position && light.follow;
                y = DrawKeyTrack(Components::Lights::GetProperty(light.id, channel), y, arrowSize,
                                 offset ? "Offset" : LightLogic::GetChannelName(channel));
            }
        }
        return y;
    }

    // A prop's track: one row with its bar, from where it shows to
    // where it goes, an item of the selection moved and trimmed as an effect's, with every key of its place on it; an
    // arrow folds out a row for its Position and one for its Turn, as a light's values
    float TimelineFrame::DrawPropTrack(const Types::Actor& actor, float y)
    {
        using namespace ImGui;
        namespace PropLogic = Components::PropLogic;
        auto drawList = GetWindowDrawList();
        const auto& style = GetStyle();
        const auto& prop = *actor.placedProp;
        const auto selected = Components::Actors::GetSelected() == actor.id;
        // (openLights holds actors' ids: a prop's fold too)
        const auto open = openLights.contains(actor.id);
        PushID(std::format("prop{}", actor.id).c_str());

        const auto arrowSize = GetFontSize();
        SetCursorScreenPos(ImVec2(headerX, y + (rowHeight - arrowSize) / 2));
        PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        if (Button(open ? ICON_FA_CARET_DOWN "##open" : ICON_FA_CARET_RIGHT "##open", ImVec2(arrowSize, arrowSize)))
        {
            if (open)
                openLights.erase(actor.id);
            else
                openLights.insert(actor.id);
        }
        PopStyleVar();
        PopStyleColor();
        AddRow(std::format("actor {} propfold", actor.id));
        SetItemTooltip(open ? "Hide its rows" : "Show a row for its Position and its Turn");

        SetCursorScreenPos(ImVec2(headerX + arrowSize + 2, y));
        if (Selectable(std::format(ICON_FA_CUBE " {}##prop", actor.name).c_str(), selected, 0,
                       ImVec2(axis.x0 - headerX - arrowSize - 8, rowHeight)))
        {
            Components::Actors::Select(actor.id);
            UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
        }
        SetItemTooltip("Drag its bar to move it, or its ends to trim it");
        AddRowName(actor);

        const auto lane = lanes.size();
        const auto row = AddLane(RowKind::Light, actor.id, y, rowHeight, std::format("actor {} prop", actor.id));
        drawList->PushClipRect(row.Min, row.Max, true);
        const auto x0 = std::floor(axis.ToX(ActorToTimeline(prop.start)));
        const auto x1 = std::floor(std::max(axis.ToX(ActorToTimeline(prop.end)), x0 + 4));
        const auto mouse = GetMousePos();
        const ImRect bar(ImVec2(x0, row.Min.y + 2), ImVec2(x1, row.Max.y - 2));
        const bool hovered = lanesHovered && drag.gesture == Gesture::None && bar.Contains(mouse);
        const auto item = TimelineSelection::BarItem(actor);
        const bool barSelected = selected || TimelineSelection::IsSelected(item);
        // (the color of props: a muted green, as the mockup has it)
        drawList->AddRectFilled(bar.Min, bar.Max,
                                hovered || barSelected ? IM_COL32(96, 130, 107, 255) : IM_COL32(78, 106, 87, 255),
                                style.FrameRounding);
        if (barSelected)
        {
            drawList->AddRect(bar.Min, bar.Max, IM_COL32(120, 165, 135, 255), style.FrameRounding, 0, 1.5f);
            drawList->AddRectFilled(bar.Min, ImVec2(bar.Min.x + 3, bar.Max.y), IM_COL32(120, 165, 135, 255));
            drawList->AddRectFilled(ImVec2(bar.Max.x - 3, bar.Min.y), bar.Max, IM_COL32(120, 165, 135, 255));
        }
        drawList->PushClipRect(bar.Min, bar.Max, true);
        drawList->AddText(ImVec2(bar.Min.x + 6, row.Min.y + style.FramePadding.y), GetColorU32(ImGuiCol_Text),
                          std::format(ICON_FA_CUBE " {}", ActorLogic::GetPropBaseName(prop.model)).c_str());
        drawList->PopClipRect();
        // (before its keys, which are found first under the mouse)
        if (bar.Max.x >= row.Min.x && bar.Min.x <= row.Max.x)
            drawn.push_back({{item}, bar, ActorToTimeline(prop.start), ActorToTimeline(prop.end), lane});

        // every key of its place
        std::map<std::uint32_t, std::vector<Item>> byTick;
        for (const auto channel : PropLogic::ALL_CHANNELS)
        {
            const auto type = Components::RuntimeProperties::FindById(PropLogic::GetPropertyId(prop.id, channel));
            if (!type || !KeyframeManager::Get().HasProperty(*type))
                continue;
            for (const auto& key : KeyframeManager::Get().GetKeyframes(KeyframeManager::Get().GetProperty(*type)))
                byTick[key.tick].push_back(TimelineSelection::KeyItem(*type, key.id));
        }
        const auto radius = std::floor(row.GetHeight() * 0.22f);
        for (const auto& [tick, items] : byTick)
        {
            const auto x = std::floor(axis.ToX(tick));
            if (x < row.Min.x - radius || x > row.Max.x + radius)
                continue;
            const ImRect hit(ImVec2(x - radius - 2, row.Min.y), ImVec2(x + radius + 2, row.Max.y));
            const bool keySelected = std::ranges::all_of(items, TimelineSelection::IsSelected);
            const bool keyHovered = lanesHovered && drag.gesture == Gesture::None && hit.Contains(mouse);
            DrawDiamond(drawList, ImVec2(x, (row.Min.y + row.Max.y) / 2), radius, GetKeyFill(keySelected, keyHovered),
                        std::nullopt);
            drawn.push_back({items, hit, tick, tick, lane});
        }
        drawList->PopClipRect();

        // its ends are grabbed a few pixels in and out
        const auto edge = EDGE_PIXELS;
        effectHandles.push_back({actor.id, bar, ImRect(ImVec2(x0 - edge, bar.Min.y), ImVec2(x0 + edge, bar.Max.y)),
                                 ImRect(ImVec2(x1 - edge, bar.Min.y), ImVec2(x1 + edge, bar.Max.y))});
        if (lanesHovered && drag.gesture == Gesture::None)
        {
            const auto& handle = effectHandles.back();
            if (handle.start.Contains(mouse) || handle.end.Contains(mouse))
                SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (handle.bar.Contains(mouse) && !IsAnyMouseDown())
                SetTooltip("%s: %s to %s", actor.name.c_str(),
                           TimelineMath::FormatTime(ActorToTimeline(prop.start)).c_str(),
                           TimelineMath::FormatTime(ActorToTimeline(prop.end)).c_str());
        }
        PopID();
        y += rowHeight + style.ItemSpacing.y;

        if (open)
        {
            for (const auto channel : PropLogic::ALL_CHANNELS)
                y = DrawKeyTrack(Components::Props::GetProperty(prop.id, channel), y, arrowSize,
                                 PropLogic::GetChannelName(channel));
        }
        return y;
    }

    TimelineFrame::GroupParts TimelineFrame::GetGroupParts(float y) const
    {
        const auto size = ImGui::GetFontSize() * 1.2f;
        const ImRect header(ImVec2(headerX, y), ImVec2(axis.x0 - 6, y + rowHeight));
        const auto middle = (header.Min.y + header.Max.y) / 2;
        const auto button = [&](float right) {
            return ImRect(ImVec2(right - size, middle - size / 2), ImVec2(right, middle + size / 2));
        };
        const auto add = button(header.Max.x - 3);
        return {header,
                ImRect(ImVec2(axis.x0, y), ImVec2(axis.x0 + axis.width, y + rowHeight)),
                ImRect(ImVec2(header.Min.x + 3, middle - size / 2), ImVec2(header.Min.x + 3 + size, middle + size / 2)),
                button(add.Min.x - 2),
                add};
    }

    void TimelineFrame::DrawGroupHits(Group group, std::span<const Types::Actor* const> rows, const GroupParts& parts)
    {
        const auto index = static_cast<std::size_t>(group);
        const auto key = "group." + TimelineGroups::GetKey(group);
        ImGui::PushID(key.c_str());
        const auto hit = [&](const char* name, const ImRect& rect, const char* tooltip) {
            ImGui::SetCursorScreenPos(rect.Min);
            const bool pressed = ImGui::InvisibleButton(name, rect.GetSize());
            AddRow(key + "." + name);
            ImGui::SetItemTooltip("%s", tooltip);
            return pressed;
        };
        if (hit("fold", parts.arrow, groupOpen[index] ? "Fold" : "Open"))
            groupOpen[index] = !groupOpen[index];
        const bool hidden = std::ranges::all_of(rows, [](const Types::Actor* row) { return row->hidden; });
        if (hit("eye", parts.eye, hidden ? "Show all" : "Hide all"))
            ToggleRowsHidden(group, rows);
        if (hit("add", parts.add, "Add one"))
        {
            if (group == Group::Effects)
                UIManager::Get().GetUIComponent<ActorsMenu>(Component::ActorsMenu)->AddEffect();
            else if (group == Group::Props)
                UIManager::Get().GetUIComponent<ActorsMenu>(Component::ActorsMenu)->AddProp();
            else
            {
                groupAdding = group;
                openGroupMenu = true;
            }
        }
        // (the rest of it folds too, and takes the mouse from a row behind it)
        if (hit("name", parts.header, groupOpen[index] ? "Fold" : "Open"))
            groupOpen[index] = !groupOpen[index];
        ImGui::PopID();
    }

    void TimelineFrame::DrawGroupHeader(Group group, std::span<const Types::Actor* const> rows, const GroupParts& parts)
    {
        const auto index = static_cast<std::size_t>(group);
        auto drawList = ImGui::GetWindowDrawList();
        // (solid: at the top, rows scroll by behind it)
        const auto fill = IM_COL32(38, 38, 38, 255);
        const auto rounding = ImGui::GetStyle().FrameRounding;
        drawList->AddRectFilled(parts.header.Min, parts.header.Max, fill, rounding);
        drawList->AddRectFilled(parts.lane.Min, parts.lane.Max, fill, rounding);
        DrawLines(parts.lane);

        const auto text = ImGui::GetColorU32(ImGuiCol_Text);
        const auto muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const auto centerY = parts.header.GetCenter().y - ImGui::GetTextLineHeight() / 2;
        const auto icon = [&](const char* glyph, const ImRect& rect, ImU32 color) {
            const auto size = ImGui::CalcTextSize(glyph);
            drawList->AddText(ImVec2(rect.GetCenter().x - size.x / 2, centerY), color, glyph);
        };
        const auto lit = [&](const ImRect& rect) {
            return ImGui::IsMouseHoveringRect(rect.Min, rect.Max) ? text : muted;
        };
        icon(groupOpen[index] ? ICON_FA_CARET_DOWN : ICON_FA_CARET_RIGHT, parts.arrow, text);
        auto x = parts.arrow.Max.x + 3;
        drawList->AddText(ImVec2(x, centerY), text, GetGroupIcon(group));
        x += ImGui::GetFontSize() * 1.3f;
        const auto name = std::string(TimelineGroups::GetName(group));
        ImGui::PushFont(UIManager::Get().GetBoldFont());
        const auto nameWidth = ImGui::CalcTextSize(name.c_str()).x;
        drawList->AddText(ImVec2(x, centerY), text, name.c_str());
        ImGui::PopFont();
        const auto count = std::to_string(rows.size());
        drawList->AddText(ImVec2(x + nameWidth + ImGui::GetFontSize() * 0.5f, centerY), muted, count.c_str());
        const bool hidden = std::ranges::all_of(rows, [](const Types::Actor* row) { return row->hidden; });
        icon(hidden ? ICON_FA_EYE_SLASH : ICON_FA_EYE, parts.eye, lit(parts.eye));
        icon(ICON_FA_PLUS, parts.add, lit(parts.add));
    }

    void TimelineFrame::DrawGroupSummary(Group group, std::span<const Types::Actor* const> rows, const ImRect& row,
                                         std::size_t lane)
    {
        constexpr float BAR_HEIGHT = 3.0f;
        constexpr float FIRST_BAR = 3.0f;
        constexpr float BAR_STEP = 3.6f;
        const auto color = group == Group::Effects       ? IM_COL32(208, 138, 60, 255)
                           : group == Group::Lights      ? IM_COL32(190, 165, 90, 255)
                           : group == Group::Props       ? IM_COL32(120, 165, 135, 255)
                           : group == Group::Killstreaks ? IM_COL32(154, 154, 154, 255)
                                                         : IM_COL32(180, 180, 180, 255);
        // (a click or a box on any of them takes everything in the group)
        std::vector<Item> items;
        for (const auto* actor : rows)
            AppendRowItems(items, *actor);
        const bool selected = !items.empty() && std::ranges::all_of(items, TimelineSelection::IsSelected);
        auto drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(row.Min, row.Max, true);
        const auto mouse = ImGui::GetMousePos();
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const auto span = TimelineGroups::GetRowSpan(*rows[i]);
            if (!span)
                continue;
            const auto start = ActorToTimeline(span->first), end = ActorToTimeline(span->second);
            const auto x0 = std::floor(axis.ToX(start)), x1 = std::floor(std::max(axis.ToX(end), axis.ToX(start) + 2));
            if (x1 < row.Min.x || x0 > row.Max.x)
                continue;
            const auto top = row.Min.y + FIRST_BAR + static_cast<float>(i % TimelineGroups::SUMMARY_LINES) * BAR_STEP;
            const ImRect hit(ImVec2(x0, row.Min.y), ImVec2(x1, row.Max.y));
            const bool hovered = lanesHovered && drag.gesture == Gesture::None && hit.Contains(mouse);
            drawList->AddRectFilled(ImVec2(x0, top), ImVec2(x1, top + BAR_HEIGHT),
                                    selected  ? ImGui::GetColorU32(ImGuiCol_CheckMark)
                                    : hovered ? IM_COL32_WHITE
                                              : color);
            drawn.push_back({items, hit, start, end, lane});
        }
        drawList->PopClipRect();
    }

    float TimelineFrame::DrawGroup(Group group, std::span<const Types::Actor* const> rows, float y, bool pinned)
    {
        const auto lane = lanes.size();
        const auto key = "group." + TimelineGroups::GetKey(group);
        const auto rect = AddLane(RowKind::Group, static_cast<std::uint32_t>(group), y, rowHeight, key, false);
        const auto parts = GetGroupParts(y);
        // (kept at the top of the tracks, it is drawn there, after the rows)
        if (!pinned)
        {
            DrawGroupHits(group, rows, parts);
            DrawGroupHeader(group, rows, parts);
        }
        if (!groupOpen[static_cast<std::size_t>(group)])
            DrawGroupSummary(group, rows, rect, lane);
        return y + rowHeight + ImGui::GetStyle().ItemSpacing.y;
    }

    void TimelineFrame::DrawGroupMenu()
    {
        if (openGroupMenu)
        {
            openGroupMenu = false;
            ImGui::OpenPopup(GROUP_MENU);
        }
        if (!ImGui::BeginPopup(GROUP_MENU))
            return;
        const auto& camera = Components::CameraManager::Get().GetActiveCamera();
        const auto position = camera->GetPosition();
        const auto forward = camera->GetForwardVector();
        const auto kindItem = [&](const char* label, Types::ActorKind kind, const char* record) {
            const auto reason = ActorActions::GetAddUnavailableReason(kind);
            if (ImGui::MenuItem(label, nullptr, false, !reason))
                ActorActions::AddActor(position, forward, kind);
            ImGuiEx::Keyframeable::Record(record);
            if (reason)
                ImGui::SetItemTooltip("%s", reason->c_str());
        };
        if (groupAdding == Group::Actors)
        {
            kindItem(ICON_FA_USER_PLUS " Actor", Types::ActorKind::Human, "group.add.actor");
        }
        else if (groupAdding == Group::Lights)
        {
            if (ImGui::MenuItem("Point"))
                ActorActions::AddLight(Types::LightKind::Point, position, forward);
            ImGuiEx::Keyframeable::Record("group.add.point");
            if (ImGui::MenuItem("Spot"))
                ActorActions::AddLight(Types::LightKind::Spot, position, forward);
            ImGuiEx::Keyframeable::Record("group.add.spot");
        }
        else
        {
            for (const auto& info : Components::KillstreakLogic::GetAll())
            {
                const auto reason = ActorActions::GetKillstreakUnavailableReason(info.killstreak);
                if (ImGui::MenuItem(std::string(info.name).c_str(), nullptr, false, !reason))
                    ActorActions::AddKillstreak(info.killstreak, position, forward);
                ImGuiEx::Keyframeable::Record("group.add." + ActorActions::GetKillstreakKey(info.killstreak));
                if (reason)
                    ImGui::SetItemTooltip("%s", reason->c_str());
            }
            const auto mortar = ActorActions::GetMortarUnavailableReason();
            if (ImGui::MenuItem(std::string(Components::KillstreakLogic::MORTAR_NAME).c_str(), nullptr, false, !mortar))
                ActorActions::AddMortar(position, forward);
            ImGuiEx::Keyframeable::Record("group.add.mortarteam");
            if (mortar)
                ImGui::SetItemTooltip("%s", mortar->c_str());
            kindItem("Dog", Types::ActorKind::Dog, "group.add.dog");
        }
        ImGui::EndPopup();
    }

    void TimelineFrame::AddRowName(const Types::Actor& actor)
    {
        AddRow(std::format("actor {} name", actor.id));
        rowNames.push_back(
            {actor.id, TimelineGroups::GetGroup(actor), ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())});
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, DRAG_PIXELS))
            draggedRow = actor.id;
    }

    // A row dragged by its name: a line where it would go among its group's rows, and it goes there as it is let go
    void TimelineFrame::HandleRowDrag()
    {
        if (!draggedRow)
            return;
        const auto moved = std::ranges::find(rowNames, *draggedRow, &RowName::actor);
        if (moved == rowNames.end())
        {
            draggedRow.reset();
            return;
        }
        const auto mouseY = ImGui::GetMousePos().y;
        std::optional<std::uint32_t> before;
        auto lineY = moved->rect.Max.y;
        for (const auto& row : rowNames)
        {
            if (row.group != moved->group)
                continue;
            lineY = row.rect.Max.y;
            if (mouseY < row.rect.GetCenter().y)
            {
                before = row.actor;
                lineY = row.rect.Min.y;
                break;
            }
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(headerX, lineY - 1), ImVec2(axis.x0 + axis.width, lineY - 1),
                                            ImGui::GetColorU32(ImGuiCol_CheckMark), 2.0f);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            return;
        Components::Actors::Reorder(TimelineGroups::MoveRow(Components::Actors::GetActors(), *draggedRow, before));
        draggedRow.reset();
    }

    float TimelineFrame::DrawActorTrack(const Types::Actor& actor, float y)
    {
        using namespace ImGui;
        auto drawList = GetWindowDrawList();
        const auto& style = GetStyle();
        const bool keyed = actor.movement == Types::ActorMovement::Keyframes;
        const bool faced = HasFaceRow(actor);
        // (an actor going along a path: its bar under the rest)
        const auto timing = actor.movement == Types::ActorMovement::Path && actor.path.size() >= 2
                                ? std::optional(Components::Actors::GetPathTiming(actor))
                                : std::nullopt;
        // (a walk a death cuts: the bar to where it dies, the rest not reached)
        const auto cut =
            timing && timing->start && !timing->arrival ? Components::Actors::GetCutAt(actor) : std::nullopt;
        const bool pathed = timing && timing->start && (timing->arrival || cut);
        const auto pathRow = std::floor(smallRow * 0.85f);
        const auto blockHeight =
            smallRow + rowHeight + (faced ? smallRow : 0.0f) + (keyed ? smallRow : 0.0f) + (pathed ? pathRow : 0.0f);
        PushID(std::format("actor{}", actor.id).c_str());

        // the header: its name, which selects it
        SetCursorScreenPos(ImVec2(headerX, y));
        const auto headerWidth = axis.x0 - headerX - 6;
        if (Selectable(std::format("{} {}##actor", GetRowIcon(actor), actor.name).c_str(),
                       Components::Actors::GetSelected() == actor.id, 0, ImVec2(headerWidth, blockHeight)))
        {
            Components::Actors::Select(actor.id);
            UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
        }
        SetItemTooltip(
            "Double-click the lane to add a clip there, right-click it for more; drag a clip to move it, or its ends "
            "to trim it. The Face row takes animations of the face; the path bar's end sets when it arrives");
        AddRowName(actor);

        // one block of rows: events along the top, the clips, the Face row's under them when it has its own face or
        // face clips, and its keys under those when it moves by keys
        const ImRect block(ImVec2(axis.x0, y), ImVec2(axis.x0 + axis.width, y + blockHeight));
        drawList->AddRectFilled(block.Min, block.Max, GetColorU32(ImGuiCol_FrameBg), style.FrameRounding);
        DrawLines(block);
        const auto eventsLane = lanes.size();
        const auto events =
            AddLane(RowKind::ActorEvents, actor.id, y, smallRow, std::format("actor {} events", actor.id), false);
        const auto clipsLane = lanes.size();
        const auto clips = AddLane(RowKind::ActorClips, actor.id, events.Max.y, rowHeight,
                                   std::format("actor {} clips", actor.id), false);
        const auto faceLane = lanes.size();
        const auto faceRect = faced ? AddLane(RowKind::ActorFace, actor.id, clips.Max.y, smallRow,
                                              std::format("actor {} face", actor.id), false)
                                    : ImRect(clips.Min, clips.Max);
        const auto keysLane = lanes.size();
        const auto keysRect = keyed ? AddLane(RowKind::ActorKeys, actor.id, faced ? faceRect.Max.y : clips.Max.y,
                                              smallRow, std::format("actor {} keys", actor.id), false)
                                    : ImRect();
        drawList->PushClipRect(block.Min, block.Max, true);
        const auto mouse = GetMousePos();

        DrawClips(actor, actor.clips, Kind::Clip, clips, clipsLane, false);
        if (actor.clips.empty())
            drawList->AddText(ImVec2(clips.Min.x + 8, clips.Min.y + style.FramePadding.y),
                              GetColorU32(ImGuiCol_TextDisabled),
                              actor.mortar ? "Its Fire events above: when the first shell lands"
                                           : "Double-click to add a clip, right-click for more");
        if (faced)
        {
            // (a faint line between it and the clips)
            drawList->AddLine(ImVec2(faceRect.Min.x, faceRect.Min.y), ImVec2(faceRect.Max.x, faceRect.Min.y),
                              IM_COL32(255, 255, 255, 18));
            DrawClips(actor, actor.faceClips, Kind::FaceClip, faceRect, faceLane, true);
            if (actor.faceClips.empty())
                drawList->AddText(GetFont(), GetFontSize() * 0.75f, ImVec2(faceRect.Min.x + 8, faceRect.Min.y + 1),
                                  GetColorU32(ImGuiCol_TextDisabled),
                                  ICON_FA_FACE_SMILE " Face: its own; drop a face animation here");
        }

        // its keys, under the clips
        if (keyed)
        {
            const auto radius = std::floor(keysRect.GetHeight() * 0.32f);
            for (const auto& key : actor.keys)
            {
                const auto at = ActorToTimeline(key.tick);
                const auto x = std::floor(axis.ToX(at));
                if (x < keysRect.Min.x - radius || x > keysRect.Max.x + radius)
                    continue;
                const Item item{Kind::ActorKey, actor.id, key.tick};
                const ImRect hit(ImVec2(x - radius - 2, keysRect.Min.y), ImVec2(x + radius + 2, keysRect.Max.y));
                const bool selected = TimelineSelection::IsSelected(item);
                const bool hovered = lanesHovered && drag.gesture == Gesture::None && hit.Contains(mouse);
                DrawDiamond(drawList, ImVec2(x, (keysRect.Min.y + keysRect.Max.y) / 2), radius,
                            GetKeyFill(selected, hovered || at == currentTick), std::nullopt);
                drawn.push_back({{item}, hit, at, at, keysLane});
            }
        }

        // its path's bar along the bottom: from when it sets off to when it arrives, each stretch in its gait's
        // colour, its waits white, a tick where it passes each point and a handle at its end, which sets when it
        // arrives (dragged, see Gesture::PathEnd); the mouse over a stretch lights it up in the view, over a point's
        // tick that point, and a click selects them as a click on the path in the view does (Gesture::PressPath)
        if (pathed)
        {
            const auto rowTop = (keyed ? keysRect.Max.y : faced ? faceRect.Max.y : clips.Max.y);
            const ImRect bar(ImVec2(axis.x0, rowTop + 3), ImVec2(axis.x0 + axis.width, rowTop + pathRow - 2));
            PathHandle handle{.actor = actor.id};
            handle.area = ImRect(ImVec2(bar.Min.x, bar.Min.y - 3), ImVec2(bar.Max.x, bar.Max.y + 3));
            handle.timing = *timing;
            const auto toX = [&](std::uint32_t tick) { return std::floor(axis.ToX(ActorToTimeline(tick))); };
            const auto plain = ImVec4(0.3f, 0.85f, 1.0f, 1.0f);
            // where its pieces are, then what the mouse is over (a point's tick before the stretches round it)
            struct Piece
            {
                std::size_t index = 0;
                ImRect rect;
                std::uint32_t from = 0, to = 0;
            };
            std::vector<Piece> pieces;
            auto& shape = handle.shape;
            for (std::size_t i = 0; i + 1 < actor.path.size(); ++i)
            {
                // (the stretch it dies on: up to its death)
                const auto to = timing->reached[i + 1]                              ? timing->reached[i + 1]
                                : cut && timing->left[i] && *cut > *timing->left[i] ? cut
                                                                                    : std::nullopt;
                if (!timing->left[i] || !to)
                    continue;
                const ImRect piece(ImVec2(toX(*timing->left[i]), bar.Min.y),
                                   ImVec2(std::max(toX(*to), toX(*timing->left[i]) + 1), bar.Max.y));
                pieces.push_back({i, piece, *timing->left[i], *to});
                shape.stretches.push_back({i, piece.Min.x, piece.Max.x});
                shape.ticks.push_back({i, piece.Min.x});
                if (timing->reached[i] && *timing->left[i] > *timing->reached[i])
                    shape.waits.push_back({i, toX(*timing->reached[i]), piece.Min.x});
                handle.stretches.emplace_back(piece, i);
            }
            if (!cut)
            {
                shape.end = toX(*timing->arrival);
                shape.endPoint = actor.path.size() - 1;
            }
            using PieceKind = TimelineMath::PathPiece::Kind;
            const bool barHovered = drag.gesture == Gesture::None && ImGui::IsWindowHovered() &&
                                    handle.area.Contains(mouse) && !OnPlayhead(mouse);
            const auto under = barHovered ? TimelineMath::PickPathBar(shape, mouse.x, TICK_PIXELS) : std::nullopt;
            // the point selected (here, in the view or the panel), and its stretch when that was clicked here
            const auto selectedPoint =
                Components::Actors::GetSelected() == actor.id ? Components::Actors::GetSelectedWaypoint() : std::nullopt;
            const bool stretchSelected = selectedPoint && pathClicked && pathClicked->first == actor.id &&
                                         pathClicked->second == TimelineMath::PathPiece{PieceKind::Stretch, *selectedPoint};
            std::optional<std::size_t> lit;
            std::optional<std::uint32_t> litTick;
            for (const auto& piece : pieces)
            {
                const auto i = piece.index;
                const auto* stretch = ActorLogic::GetStretch(actor, i);
                const bool hovered = under && under->kind == PieceKind::Stretch && under->index == i;
                auto color = stretch ? GetGaitColor(stretch->gait, stretch->crouched) : plain;
                color.w = hovered ? 1.0f : 0.85f;
                drawList->AddRectFilled(piece.rect.Min, piece.rect.Max, GetColorU32(color));
                // (its look is named on the walk's piece above it)
                if (hovered)
                {
                    drawList->AddRect(piece.rect.Min, piece.rect.Max, IM_COL32_WHITE);
                    lit = i;
                    const auto name = stretch ? ActorActions::GetGaitName(actor, stretch->gait, stretch->crouched)
                                              : std::string("Walk");
                    SetTooltip(
                        "%s to point %zu: %s, %.2f s\nLook: %s%s\nClick to select it, double-click to go there, "
                        "drag to slide the walk",
                        i == 0 ? "Start" : std::format("Point {}", i).c_str(), i + 1, name.c_str(),
                        (piece.to - piece.from) / 1000.0f, stretch ? stretch->animation.c_str() : "",
                        stretch && !stretch->look.empty() ? " (its own)" : "");
                }
                // selected: outlined
                if (stretchSelected && *selectedPoint == i)
                    drawList->AddRect(ImVec2(piece.rect.Min.x - 1, piece.rect.Min.y - 1),
                                      ImVec2(piece.rect.Max.x + 1, piece.rect.Max.y + 1), IM_COL32_WHITE, 0.0f, 0, 2.0f);
                // a wait at its first point
                if (timing->reached[i] && *timing->left[i] > *timing->reached[i])
                    drawList->AddRectFilled(ImVec2(toX(*timing->reached[i]), bar.Min.y),
                                            ImVec2(piece.rect.Min.x, bar.Max.y),
                                            GetColorU32(ImVec4(WAIT_COLOR.x, WAIT_COLOR.y, WAIT_COLOR.z, 0.8f)));
                // the tick where it sets off from its point: white and the row's height when that point is selected
                const bool tickHovered =
                    under && (under->kind == PieceKind::Point || under->kind == PieceKind::Wait) && under->index == i;
                if (selectedPoint == i && !stretchSelected)
                    drawList->AddRectFilled(ImVec2(piece.rect.Min.x - 1, handle.area.Min.y),
                                            ImVec2(piece.rect.Min.x + 1, handle.area.Max.y), IM_COL32_WHITE);
                else
                    drawList->AddLine(ImVec2(piece.rect.Min.x, bar.Min.y - 2), ImVec2(piece.rect.Min.x, bar.Max.y + 1),
                                      tickHovered ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 160));
            }
            // what a change on the walk's piece shown in the panel reaches (Apply to):
            // its stretches, or the whole walk
            if (const auto scope = UIManager::Get().GetUIComponent<ActorsMenu>(Component::ActorsMenu)->GetPieceScope();
                scope && scope->actor == actor.id)
            {
                auto from = FLT_MAX, to = -FLT_MAX;
                for (const auto& piece : pieces)
                    if (scope->wholePath || (piece.index >= scope->first && piece.index <= scope->last))
                    {
                        from = std::min(from, piece.rect.Min.x);
                        to = std::max(to, piece.rect.Max.x);
                    }
                if (from < to)
                    drawList->AddRect(ImVec2(from - 1, bar.Min.y - 1), ImVec2(to + 1, bar.Max.y + 1), IM_COL32_WHITE,
                                      0.0f, 0, 2.0f);
            }
            // a faint line from each join on the walk down to its point's tick
            for (const auto& piece : pieceHandles)
                if (piece.actor == actor.id && piece.pointX)
                    for (auto y = piece.rect.Max.y + 1.0f; y < bar.Min.y - 1.0f; y += 4.0f)
                        drawList->AddLine(ImVec2(*piece.pointX + 0.5f, y),
                                          ImVec2(*piece.pointX + 0.5f, std::min(y + 2.0f, bar.Min.y - 1.0f)),
                                          IM_COL32(255, 255, 255, 90));
            // a point under the mouse: named, with when it gets there, and lit in the view
            if (under && (under->kind == PieceKind::Point || under->kind == PieceKind::Wait))
            {
                const auto at = GetPathPieceTick(handle, *under);
                SetTooltip(
                    "%s \xc2\xb7 %s\nClick to select it, double-click to go there, drag to slide the walk "
                    "(its tick snaps to the playhead)",
                    under->index == 0 ? "Start" : std::format("Point {}", under->index).c_str(),
                    at ? TimelineMath::FormatTime(ActorToTimeline(*at)).c_str() : "");
                lit = std::min(under->index, actor.path.size() - 2);
                litTick = at;
            }
            // where it is along the stretch under the mouse, in the view
            if (lit)
                GFX::GraphicsManager::Get().SetHighlightedStretch(
                    std::pair(actor.id, *lit), litTick ? *litTick : TimelineToActor(std::llround(axis.ToTick(mouse.x))));
            if (cut)
            {
                // not reached: hatched after its death, and no end to drag
                const ImRect rest(ImVec2(toX(*cut), bar.Min.y), ImVec2(toX(*cut) + GetFontSize() * 3.0f, bar.Max.y));
                drawList->PushClipRect(rest.Min, rest.Max, true);
                for (auto x = rest.Min.x - rest.GetHeight(); x < rest.Max.x; x += 5.0f)
                    drawList->AddLine(ImVec2(x, rest.Max.y), ImVec2(x + rest.GetHeight(), rest.Min.y),
                                      IM_COL32(200, 200, 200, 120), 1.0f);
                drawList->PopClipRect();
                if (drag.gesture == Gesture::None && ImGui::IsWindowHovered() && rest.Contains(mouse))
                    SetTooltip("Dies at %.2f s, before the path's end: the rest isn't reached",
                               ActorToTimeline(*cut) / 1000.0f);
                handle.bar = ImRect(ImVec2(toX(*timing->start), bar.Min.y), ImVec2(toX(*cut), bar.Max.y));
            }
            else
            {
                const auto endX = toX(*timing->arrival);
                handle.bar = ImRect(ImVec2(toX(*timing->start), bar.Min.y), ImVec2(endX, bar.Max.y));
                handle.end = ImRect(ImVec2(endX - 5, bar.Min.y - 3), ImVec2(endX + 5, bar.Max.y + 3));
                const bool endHovered =
                    drag.gesture == Gesture::None && ImGui::IsWindowHovered() && handle.end.Contains(mouse);
                // (selected: its last point)
                const bool endSelected = selectedPoint && *selectedPoint + 1 == actor.path.size();
                drawList->AddRectFilled(ImVec2(endX - 2, bar.Min.y - 3), ImVec2(endX + 2, bar.Max.y + 3),
                                        endHovered || endSelected ? IM_COL32_WHITE : IM_COL32(230, 230, 230, 220));
                if (endSelected)
                    drawList->AddRect(ImVec2(endX - 3, bar.Min.y - 4), ImVec2(endX + 3, bar.Max.y + 4),
                                      GetColorU32(ImGuiCol_CheckMark));
                if (endHovered)
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    SetTooltip("Arrives at %.2f s: drag to change when (its pace follows); click to select its last "
                               "point",
                               *timing->arrival / 1000.0f);
                }
                // dragged: where it would arrive, and at what pace
                if (drag.gesture == Gesture::PathEnd && drag.pathActor == actor.id)
                {
                    const auto x = std::floor(axis.ToX(static_cast<double>(drag.pathTick)));
                    drawList->AddRectFilled(ImVec2(x - 2, bar.Min.y - 3), ImVec2(x + 2, bar.Max.y + 3),
                                            GetColorU32(ImGuiCol_CheckMark));
                    drawList->AddLine(ImVec2(x, block.Min.y), ImVec2(x, block.Max.y),
                                      GetColorU32(ImGuiCol_CheckMark, 0.6f));
                }
            }
            timeline.pathBars.push_back({actor.id, bar.Min.y, bar.Max.y, shape});
            pathHandles.push_back(std::move(handle));
        }

        // its events along the top, a burst of shots as a bar to its last shot, a faint line down to where it is
        const auto iconSize = CalcTextSize(ICON_FA_GUN) * 0.75f;
        for (const auto& event : actor.events)
        {
            const auto at = ActorToTimeline(event.tick), end = ActorToTimeline(event.End());
            const auto x = std::floor(axis.ToX(at));
            const auto endX = std::min(axis.ToX(end), block.Max.x);
            if (endX < block.Min.x || x > block.Max.x)
                continue;
            const Item item{Kind::Event, actor.id, event.id};
            const bool selected = TimelineSelection::IsSelected(item);
            const ImRect rect(ImVec2(x - iconSize.x / 2 - 1, events.Min.y),
                              ImVec2(x + iconSize.x / 2 + 1, events.Max.y));
            const bool hovered = lanesHovered && drag.gesture == Gesture::None && rect.Contains(mouse);
            const auto color = selected ? GetColorU32(ImGuiCol_CheckMark) : hovered ? IM_COL32_WHITE : EVENT_COLOR;
            if (endX > x + 1)
                drawList->AddRectFilled(ImVec2(std::max(x, block.Min.x), events.Max.y - 3),
                                        ImVec2(endX, events.Max.y - 1), color);
            drawList->AddLine(ImVec2(x, events.Max.y), ImVec2(x, block.Max.y), WithAlpha(color, 90));
            drawList->AddRectFilled(rect.Min, rect.Max, GetColorU32(ImGuiCol_FrameBg), 2.0f);
            drawList->AddText(GetFont(), GetFontSize() * 0.75f, ImVec2(rect.Min.x + 1, rect.Min.y + 1), color,
                              DescribeEventIcon(event.type));
            drawn.push_back({{item}, rect, at, end, eventsLane});
        }
        drawList->PopClipRect();
        PopID();
        return y + blockHeight + style.ItemSpacing.y;
    }

    void TimelineFrame::DrawClips(const Types::Actor& actor, const std::vector<Types::ActorClip>& clips, Kind kind,
                                  const ImRect& row, std::size_t lane, bool compact)
    {
        using namespace ImGui;
        auto drawList = GetWindowDrawList();
        const auto& style = GetStyle();
        const auto mouse = GetMousePos();
        // (a walk of the path in its pieces)
        const auto pieces =
            kind == Kind::Clip ? Components::Actors::GetWalkPieces(actor) : std::vector<ActorLogic::WalkPiece>{};
        // a clip shows until the next one starts, which takes over from it
        for (std::size_t i = 0; i < clips.size(); ++i)
        {
            const auto& clip = clips[i];
            const auto visibleEnd = i + 1 < clips.size() ? std::min(clip.End(), clips[i + 1].start) : clip.End();
            if (visibleEnd <= clip.start)
                continue;
            if (std::ranges::any_of(pieces, [&](const ActorLogic::WalkPiece& piece) { return piece.clip == clip.id; }))
            {
                DrawWalkPieces(actor, clip, pieces, row, lane);
                continue;
            }
            const auto start = ActorToTimeline(clip.start), end = ActorToTimeline(clip.End());
            const auto x0 = std::max(axis.ToX(start), row.Min.x - 4);
            const auto x1 = std::min(axis.ToX(ActorToTimeline(visibleEnd)), row.Max.x + 4);
            if (x1 < row.Min.x || x0 > row.Max.x || x1 <= x0)
                continue;
            const Item item{kind, actor.id, clip.id};
            const ImRect rect(ImVec2(std::floor(x0), row.Min.y + 1),
                              ImVec2(std::floor(std::max(x1, x0 + 2)), row.Max.y - 1));
            const bool selected = TimelineSelection::IsSelected(item);
            const bool hovered = lanesHovered && drag.gesture == Gesture::None && rect.Contains(mouse);
            // (those Auto transitions put in are paler, marked with a link)
            const bool chained = clip.chain != Types::ActorChain::None;
            drawList->AddRectFilled(rect.Min, rect.Max,
                                    GetColorU32(selected  ? ImGuiCol_ButtonActive
                                                : hovered ? ImGuiCol_ButtonHovered
                                                          : ImGuiCol_Button,
                                                chained && !selected && !hovered ? 0.55f : 1.0f),
                                    style.FrameRounding);
            // the blend in, as a ramp in the clip's bottom pixels under its name, rising from the clip's start to where
            // it has taken over (no gradient)
            if (clip.blendIn > 0)
            {
                const auto blendX = std::min(axis.ToX(ActorToTimeline(clip.start + clip.blendIn)), rect.Max.x);
                const auto height = compact ? 3.0f : 5.0f;
                // (clockwise, as ImGui's anti-aliasing wants it)
                if (blendX > rect.Min.x + 1)
                    drawList->AddTriangleFilled(ImVec2(rect.Min.x, rect.Max.y), ImVec2(blendX, rect.Max.y - height),
                                                ImVec2(blendX, rect.Max.y), IM_COL32(255, 255, 255, 110));
            }
            if (selected)
                drawList->AddRect(rect.Min, rect.Max, GetColorU32(ImGuiCol_Text), style.FrameRounding);
            // put in or moved just now by planning the path again: a flash
            if (chained && kind == Kind::Clip)
                for (const auto& flash : flashes)
                    if (flash.actor == actor.id && std::ranges::find(flash.clips, clip.id) != flash.clips.end())
                    {
                        const auto age =
                            std::chrono::duration<float>(std::chrono::steady_clock::now() - flash.at).count();
                        if (age < 0.6f)
                            drawList->AddRect(ImVec2(rect.Min.x - 1, rect.Min.y - 1),
                                              ImVec2(rect.Max.x + 1, rect.Max.y + 1),
                                              IM_COL32(255, 255, 255, static_cast<int>(255 * (1.0f - age / 0.6f))),
                                              style.FrameRounding, 0, 2.0f);
                    }
            drawList->PushClipRect(rect.Min, rect.Max, true);
            // (a killstreak's motion by its name: Fly, Hover...; a walk of a path by its look)
            const auto shownName = Components::KillstreakLogic::IsMotion(clip.animation)
                                       ? std::string(Components::KillstreakLogic::GetMotionLabel(clip.animation))
                                   : kind == Kind::Clip ? Components::Actors::GetShownAnimation(actor, clip)
                                                        : clip.animation;
            // (one that hangs on the path's end or a point: an anchor before its name)
            const bool anchored = kind == Kind::Clip && clip.anchor != Types::ActorAnchor::Time;
            const auto label =
                std::string(chained ? ICON_FA_LINK " " : "") + (anchored ? ICON_FA_ANCHOR " " : "") + shownName;
            if (compact)
                drawList->AddText(GetFont(), GetFontSize() * 0.75f, ImVec2(rect.Min.x + 4, row.Min.y + 1),
                                  GetColorU32(ImGuiCol_Text), label.c_str());
            else
                drawList->AddText(ImVec2(rect.Min.x + 4, row.Min.y + style.FramePadding.y), GetColorU32(ImGuiCol_Text),
                                  label.c_str());
            drawList->PopClipRect();
            // (taken a little past its ends, which trim it)
            const bool trimEnd = visibleEnd == clip.End();
            const ImRect hit(ImVec2(rect.Min.x - EDGE_PIXELS, rect.Min.y),
                             ImVec2(rect.Max.x + (trimEnd ? EDGE_PIXELS : 0.0f), rect.Max.y));
            drawn.push_back({{item}, hit, start, end, lane, true, trimEnd, rect.Min.x, rect.Max.x});
            // what it hangs on, as a thin line down to it on the path bar (its end, or where it reaches the point), and
            // in its tooltip
            if (anchored)
            {
                const auto timing = Components::Actors::GetPathTiming(actor);
                const auto at = clip.anchor == Types::ActorAnchor::End     ? timing.arrival
                                : clip.anchorPoint < timing.reached.size() ? timing.reached[clip.anchorPoint]
                                                                           : std::nullopt;
                if (at)
                {
                    const auto x = std::floor(axis.ToX(ActorToTimeline(*at)));
                    const auto y = rect.Max.y + 1;
                    drawList->AddLine(ImVec2(rect.Min.x + 1, y), ImVec2(x, y + GetFontSize() * 0.9f),
                                      IM_COL32(255, 255, 255, 140), 1.0f);
                    drawList->AddCircleFilled(ImVec2(x, y + GetFontSize() * 0.9f), 2.5f, IM_COL32(255, 255, 255, 200));
                }
            }
        }
    }

    // Each piece named by its look, from where its join's blend starts (the clip's start for its first), a ramp in its
    // bottom pixels over that blend as clips' blends in are drawn (8.8; no gradient); they don't trim (the path sets
    // them) and the walk moves as one clip. The piece the panel shows (the selected point's) is outlined, else every
    // piece of a selected walk.
    void TimelineFrame::DrawWalkPieces(const Types::Actor& actor, const Types::ActorClip& clip,
                                       const std::vector<ActorLogic::WalkPiece>& pieces, const ImRect& row,
                                       std::size_t lane)
    {
        using namespace ImGui;
        auto drawList = GetWindowDrawList();
        const auto& style = GetStyle();
        const auto mouse = GetMousePos();
        const Item item{Kind::Clip, actor.id, clip.id};
        const bool selected = TimelineSelection::IsSelected(item);
        const bool chained = clip.chain != Types::ActorChain::None;
        const auto timing = Components::Actors::GetPathTiming(actor);
        const auto point =
            Components::Actors::GetSelected() == actor.id ? Components::Actors::GetSelectedWaypoint() : std::nullopt;
        const bool pointIn = point && std::ranges::any_of(pieces, [&](const ActorLogic::WalkPiece& piece) {
                                 return piece.clip == clip.id && *point >= piece.first && *point <= piece.last;
                             });
        for (std::size_t i = 0; i < pieces.size(); ++i)
        {
            const auto& piece = pieces[i];
            if (piece.clip != clip.id)
                continue;
            const auto start = ActorToTimeline(piece.start), end = ActorToTimeline(piece.end);
            const auto x0 = std::max(axis.ToX(start), row.Min.x - 4);
            const auto x1 = std::min(axis.ToX(end), row.Max.x + 4);
            if (x1 < row.Min.x || x0 > row.Max.x || x1 <= x0)
                continue;
            // (a pixel apart from the piece before)
            const ImRect rect(ImVec2(std::floor(x0) + (piece.joined ? 1.0f : 0.0f), row.Min.y + 1),
                              ImVec2(std::floor(std::max(x1, x0 + 2)), row.Max.y - 1));
            const bool hovered = lanesHovered && drag.gesture == Gesture::None && rect.Contains(mouse);
            drawList->AddRectFilled(rect.Min, rect.Max,
                                    GetColorU32(selected  ? ImGuiCol_ButtonActive
                                                : hovered ? ImGuiCol_ButtonHovered
                                                          : ImGuiCol_Button,
                                                chained && !selected && !hovered ? 0.55f : 1.0f),
                                    style.FrameRounding);
            const auto rampX = std::min(axis.ToX(ActorToTimeline(piece.start + piece.blend)), rect.Max.x);
            if (piece.blend > 0 && rampX > rect.Min.x + 1)
                drawList->AddTriangleFilled(ImVec2(rect.Min.x, rect.Max.y), ImVec2(rampX, rect.Max.y - 5.0f),
                                            ImVec2(rampX, rect.Max.y), IM_COL32(255, 255, 255, 110));
            const ImRect ramp(rect.Min, ImVec2(std::max(rampX, rect.Min.x), rect.Max.y));
            if (selected && (!pointIn || (*point >= piece.first && *point <= piece.last)))
                drawList->AddRect(rect.Min, rect.Max, GetColorU32(ImGuiCol_Text), style.FrameRounding);
            drawList->PushClipRect(rect.Min, rect.Max, true);
            const auto label = std::string(chained ? ICON_FA_LINK " " : "") + piece.animation;
            drawList->AddText(ImVec2(rect.Min.x + 4, row.Min.y + style.FramePadding.y), GetColorU32(ImGuiCol_Text),
                              label.c_str());
            drawList->PopClipRect();
            // (a joined piece: its point's tick, which a double-click goes to)
            std::optional<float> pointX;
            std::optional<std::int64_t> jump = start;
            if (piece.joined && piece.first < timing.reached.size() && timing.reached[piece.first])
            {
                jump = ActorToTimeline(*timing.reached[piece.first]);
                pointX = std::floor(axis.ToX(static_cast<double>(*jump)));
            }
            drawn.push_back({{item}, rect, start, end, lane, false, false, rect.Min.x, rect.Max.x, piece.first, jump});
            pieceHandles.push_back({actor.id, i, piece, rect, ramp, pointX});
            timeline.walkPieces.push_back(
                {actor.id, i, piece.animation, rect.Min.x, rect.Max.x, rect.Min.y, rect.Max.y, ramp.Min.x, ramp.Max.x});
        }
    }

    void TimelineFrame::DrawTracks(ImVec2 size)
    {
        auto& io = ImGui::GetIO();
        // the wheel scrolls the tracks; with Shift it pans and with Ctrl it zooms (HandleInput)
        ImGuiWindowFlags flags = ImGuiWindowFlags_None;
        if (io.KeyCtrl || io.KeyShift || drag.gesture != Gesture::None)
            flags |= ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild("##tracks", size, false, flags);
        const auto window = ImGui::GetCurrentWindow();
        // (each frame's: the path bars and walks' pieces drawn, the stretch the mouse is over, the flashes)
        pathHandles.clear();
        pieceHandles.clear();
        effectHandles.clear();
        GFX::GraphicsManager::Get().SetHighlightedStretch(std::nullopt);
        flashes = Components::Actors::GetFlashes();
        const auto mouse = ImGui::GetMousePos();
        lanesView = ImRect(ImVec2(axis.x0, window->InnerClipRect.Min.y),
                           ImVec2(axis.x0 + axis.width, window->InnerClipRect.Max.y));
        lanesClip = lanesView;
        headerX = window->WorkRect.Min.x;
        rowNames.clear();

        auto y = ImGui::GetCursorScreenPos().y;
        const auto top = y;
        EndActorEditStepWhenIdle();

        const auto actors = Components::Actors::GetActors();
        // one picked in a folded group opens it
        if (const auto picked = Components::Actors::GetSelected(); picked != seenPicked)
        {
            seenPicked = picked;
            if (const auto actor = std::ranges::find(actors, picked.value_or(0), &Types::Actor::id);
                picked && actor != actors.end())
                groupOpen[static_cast<std::size_t>(TimelineGroups::GetGroup(*actor))] = true;
        }

        // the header of the group whose rows are scrolling by stays at the top: it takes the mouse first, as what is
        // submitted first does, and from the rows behind it, and is drawn over them after them
        std::array<std::optional<TimelineGroups::Span>, GROUP_COUNT> spans;
        for (std::size_t i = 0; i < GROUP_COUNT; ++i)
        {
            if (groupSpans[i] && groupOpen[i])
                spans[i] = TimelineGroups::Span{top + groupSpans[i]->top, top + groupSpans[i]->bottom};
        }
        const auto pin = TimelineGroups::GetPin(spans, lanesView.Min.y, rowHeight);
        const auto pinnedRows = pin ? TimelineGroups::GetRows(actors, TimelineGroups::ALL[pin->group])
                                    : std::vector<const Types::Actor*>{};
        if (pin && !pinnedRows.empty())
        {
            lanesClip.Min.y = std::max(lanesClip.Min.y, pin->y + rowHeight);
            DrawGroupHits(TimelineGroups::ALL[pin->group], pinnedRows, GetGroupParts(pin->y));
            // folded from there, its own header comes to where it was, or it would be gone above the rows
            if (!groupOpen[pin->group])
                ImGui::SetScrollY(ImGui::GetScrollY() + spans[pin->group]->top - lanesView.Min.y);
        }
        lanesHovered = ImGui::IsWindowHovered() && lanesClip.Contains(mouse);
        timeline.rows.push_back({"lanes", lanesClip.Min, lanesClip.Max});

        y = DrawKeyTrack(GetCampathProperty(), y, 0.0f);
        y = DrawTrackGroup(y, true);
        y = DrawTrackGroup(y, false);
        // the rows by type, each group as it was placed, without those with no rows
        for (std::size_t i = 0; i < GROUP_COUNT; ++i)
        {
            const auto group = TimelineGroups::ALL[i];
            const auto rows = TimelineGroups::GetRows(actors, group);
            groupSpans[i].reset();
            if (rows.empty())
                continue;
            const auto groupTop = y;
            y = DrawGroup(group, rows, y, pin && pin->group == i);
            for (const auto* actor : rows)
            {
                if (groupOpen[i])
                    y = actor->effect       ? DrawEffectTrack(*actor, y)
                        : actor->light      ? DrawLightTrack(*actor, y)
                        : actor->placedProp ? DrawPropTrack(*actor, y)
                                            : DrawActorTrack(*actor, y);
            }
            groupSpans[i] = TimelineGroups::Span{groupTop - top, y - top};
        }
        if (pin && !pinnedRows.empty())
            DrawGroupHeader(TimelineGroups::ALL[pin->group], pinnedRows, GetGroupParts(pin->y));
        HandleRowDrag();
        DrawGroupMenu();

        // adding a track
        ImGui::SetCursorScreenPos(ImVec2(headerX, y));
        if (ImGui::Button(ICON_FA_PLUS " Track"))
            ImGui::OpenPopup(TRACK_MENU);
        tracksHeight = ImGui::GetItemRectMax().y - top + ImGui::GetStyle().WindowPadding.y;
        ImGui::SetItemTooltip("Show a track, to key it");
        if (ImGui::BeginPopup(TRACK_MENU))
        {
            const auto item = [](const Types::KeyframeableProperty& property, const std::vector<Types::Keyframe>& keys,
                                 bool& open) {
                const bool shown = IsShown(property, keys);
                if (ImGui::MenuItem(property.name.data(), nullptr, shown, keys.empty()))
                {
                    if (shown)
                        shownProperties.erase(property.type);
                    else
                    {
                        shownProperties.insert(property.type);
                        open = true;
                    }
                }
            };
            const auto features = Mod::GetGameInterface()->GetSupportedFeatures();
            for (const auto& [property, keys] : KeyframeManager::Get().GetKeyframes())
            {
                // (those the game has no control for, but with keys an older file gave them)
                if (property.type == Types::KeyframeablePropertyType::CampathCamera ||
                    Components::BoneCameraLogic::IsKeyedProperty(property.type) ||
                    Components::RuntimeProperties::IsActorProperty(property.type) ||
                    (!Components::VisualsLogic::IsUsed(property.type, features) && keys.empty()))
                    continue;
                item(property, keys, lookOpen);
            }
            ImGui::Separator();
            for (const auto type : Components::BoneCameraLogic::KEYED_PROPERTIES)
            {
                const auto& property = KeyframeManager::Get().GetProperty(type);
                item(property, KeyframeManager::Get().GetKeyframes(property), boneCameraOpen);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_PERSON " Actor (on the Scene tab)"))
                UIManager::Get().SelectTab(Tab::Scene, Page::Actors);
            ImGui::EndPopup();
        }

        HandleInput();
        HandleAnimationDrop();
        DrawMenus();
        DrawOverlays();
        ImGui::EndChild();
    }

    // An animation dragged from the animation browser (UI::AnimationBrowser) onto an actor's lane: where a play of it
    // would go, snapping as a drag does, and dropped there, a clip of it (ActorActions::Play); the browser over the
    // timeline stays open for more
    void TimelineFrame::HandleAnimationDrop()
    {
        const auto payload = ImGui::GetDragDropPayload();
        if (!payload || !payload->IsDataType(ANIMATION_PAYLOAD))
            return;
        const auto mouse = ImGui::GetMousePos();
        const auto lane = std::ranges::find_if(lanes, [&](const Lane& each) {
            return (each.kind == RowKind::ActorEvents || each.kind == RowKind::ActorClips ||
                    each.kind == RowKind::ActorFace || each.kind == RowKind::ActorKeys) &&
                   each.rect.Contains(mouse) && lanesClip.Contains(mouse);
        });
        // (the browser's popup keeps the timeline from being hovered as usual; a custom target looks at the window
        // under the mouse)
        if (lane == lanes.end() ||
            !ImGui::BeginDragDropTargetCustom(lane->rect, ImGui::GetID(std::format("drop{}", lane->owner).c_str())))
            return;
        const std::string animation(static_cast<const char*>(payload->Data));
        const auto owner = lane->owner;
        CollectSnapTargets(axis, {}, true);
        const auto tick = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(SnapTick(timeline, axis, std::llround(axis.ToTick(mouse.x))), 0, endTick));

        // a play of it from there, on the actor's clips; of the face, on its Face row (shown where the clips are
        // while it has none)
        const auto catalog = Components::Actors::GetCatalog();
        const auto found = catalog ? catalog->FindAnimation(animation) : nullptr;
        const bool face = found && found->kind == Types::AnimationKind::Face;
        const auto length = found ? std::max(1.0f, found->duration * 1000.0f) : 1000.0f;
        auto clips = std::ranges::find_if(lanes, [&](const Lane& each) {
            return each.kind == (face ? RowKind::ActorFace : RowKind::ActorClips) && each.owner == owner;
        });
        if (clips == lanes.end())
            clips = std::ranges::find_if(
                lanes, [&](const Lane& each) { return each.kind == RowKind::ActorClips && each.owner == owner; });
        if (clips != lanes.end())
        {
            auto drawList = ImGui::GetWindowDrawList();
            const ImRect ghost(
                ImVec2(std::floor(axis.ToX(tick)), clips->rect.Min.y + 1),
                ImVec2(std::floor(std::max(axis.ToX(tick + length), axis.ToX(tick) + 4)), clips->rect.Max.y - 1));
            drawList->PushClipRect(lanesView.Min, lanesView.Max, true);
            drawList->AddRectFilled(ghost.Min, ghost.Max, IM_COL32(110, 210, 110, 90), ImGui::GetStyle().FrameRounding);
            drawList->AddRect(ghost.Min, ghost.Max, IM_COL32(110, 210, 110, 255), ImGui::GetStyle().FrameRounding);
            const auto text = std::format("{}{} at {}", face ? ICON_FA_FACE_SMILE " " : "", animation,
                                          TimelineMath::FormatTime(tick));
            if (clips->kind == RowKind::ActorFace)
                drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.75f,
                                  ImVec2(ghost.Min.x + 4, clips->rect.Min.y + 1), ImGui::GetColorU32(ImGuiCol_Text),
                                  text.c_str());
            else
                drawList->AddText(ImVec2(ghost.Min.x + 4, clips->rect.Min.y + ImGui::GetStyle().FramePadding.y),
                                  ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
            if (snappedTick)
                drawList->AddLine(ImVec2(ghost.Min.x, lanesView.Min.y), ImVec2(ghost.Min.x, lanesView.Max.y),
                                  SNAP_COLOR);
            drawList->PopClipRect();
        }
        if (ImGui::AcceptDragDropPayload(ANIMATION_PAYLOAD, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
        {
            const auto actor = Components::Actors::GetActor(owner);
            // (one that doesn't fit, or a face on a dog, says so instead)
            const bool fits = actor && Components::Actors::Fits(actor->models, animation).value_or(true) &&
                              !(face && Components::Actors::GetActorKind(*actor) == Types::ActorKind::Dog);
            if (face)
                ActorActions::PlayFace(owner, animation, TimelineToActor(tick));
            else
                ActorActions::Play(owner, animation, TimelineToActor(tick));
            if (actor && fits)
                Toasts::Show(std::format("{} for {}{} at {}", animation, actor->name, face ? "'s face" : "",
                                         Toasts::FormatTime(tick)),
                             true);
        }
        ImGui::EndDragDropTarget();
    }

    void TimelineFrame::DrawOverlays()
    {
        auto drawList = ImGui::GetWindowDrawList();
        drawList->PushClipRect(lanesView.Min, lanesView.Max, true);
        const auto line = [&](std::int64_t tick, ImU32 color, float thickness) {
            const auto x = std::floor(axis.ToX(static_cast<double>(tick)));
            drawList->AddLine(ImVec2(x, lanesView.Min.y), ImVec2(x, lanesView.Max.y), color, thickness);
        };
        // through every lane: the capture range's ends, the frozen tick, the playhead
        const auto [captureStart, captureEnd] = GetCaptureRange();
        line(captureStart, WithAlpha(CAPTURE_COLOR, 150), 1.0f);
        line(captureEnd, WithAlpha(CAPTURE_COLOR, 150), 1.0f);
        if (const auto frozen = Components::Playback::GetFrozenTick())
            line(*frozen, WithAlpha(FROZEN_COLOR, 170), 1.0f);
        // bullet time's spans, faintly, where the demo stands still
        {
            const auto spans = Components::Playback::GetFrozenSpans();
            for (std::size_t i = 0; i < spans->size(); ++i)
            {
                const auto start = Components::FrozenSpans::GetStart(*spans, i);
                drawList->AddRectFilled(ImVec2(std::floor(axis.ToX(start)), lanesView.Min.y),
                                        ImVec2(std::floor(axis.ToX(start + (*spans)[i].length)), lanesView.Max.y),
                                        WithAlpha(SPAN_COLOR, 24));
            }
        }
        line(currentTick, Theme::Playhead(), 2.0f);
        if (snappedTick &&
            (drag.gesture == Gesture::Move || drag.gesture == Gesture::Scrub || drag.gesture == Gesture::Trim ||
             drag.gesture == Gesture::Range || drag.gesture == Gesture::SpanEnd || drag.gesture == Gesture::SlidePath))
            line(*snappedTick, SNAP_COLOR, 1.0f);

        if (drag.gesture == Gesture::Box)
        {
            const auto mouse = ImGui::GetMousePos();
            const ImVec2 from(std::floor(axis.ToX(drag.pressTick)), drag.press.y);
            const ImRect box(ImMin(from, mouse), ImMax(from, mouse));
            drawList->AddRectFilled(box.Min, box.Max, Theme::SelectionFill());
            drawList->AddRect(box.Min, box.Max, Theme::SelectionEdge());
        }
        drawList->PopClipRect();
    }

    void TimelineFrame::DrawTooltip(const Drawn& item)
    {
        const auto& first = item.items.front();
        const auto time = TimelineMath::FormatTime(item.start);
        std::string text;
        if (item.items.size() > 1)
        {
            text = std::format("{} at {} (tick {})", TimelineSelection::Describe(item.items), time, item.start);
        }
        else if (first.kind == Kind::Key)
        {
            const bool campath =
                first.owner == static_cast<std::uint32_t>(Types::KeyframeablePropertyType::CampathCamera);
            text = std::format("{} at {} (tick {})\nDrag to move, double-click to go there, right-click for more",
                               campath ? "Node" : "Key", time, item.start);
        }
        else if (first.kind == Kind::Marker)
        {
            const auto marker = Components::Markers::Find(static_cast<std::uint32_t>(first.id));
            text = std::format("{} at {} (tick {})\nDrag to move, double-click to rename, right-click for more",
                               marker ? marker->name : "Marker", time, item.start);
        }
        else if (TimelineSelection::IsBar(first))
        {
            // (its track says what it is)
        }
        else if (const auto actor = Components::Actors::GetActor(first.owner))
        {
            // a piece of a walk: its look, gait and speed, its stretches and its blend
            const auto piece = std::ranges::find_if(pieceHandles, [&](const PieceHandle& handle) {
                return item.stretch && handle.actor == actor->id && handle.piece.first == *item.stretch &&
                       handle.piece.clip == static_cast<std::uint32_t>(first.id);
            });
            if (first.kind == Kind::Clip && piece != pieceHandles.end())
            {
                const auto& walk = piece->piece;
                const auto* stretch = ActorLogic::GetStretch(*actor, walk.first);
                const auto speed =
                    stretch ? ActorLogic::GetStretchSpeed(*actor, *stretch, Components::Actors::GetAnimationLookup())
                            : 0.0f;
                const auto distance =
                    walk.joined ? Components::Actors::GetJoinDistance(*actor, walk.first) : std::nullopt;
                text = std::format(
                    "{} \xc2\xb7 {}, {:.0f} u/s{}\n{} " ICON_FA_ARROW_RIGHT
                    " point {}{}\nClick to select it, "
                    "double-click to go there{}",
                    walk.animation,
                    stretch ? ActorActions::GetGaitName(*actor, stretch->gait, stretch->crouched) : std::string("Walk"),
                    speed, stretch && !stretch->look.empty() ? " \xc2\xb7 its own look" : "",
                    walk.first == 0 ? std::string("Start") : std::format("Point {}", walk.first), walk.last + 1,
                    walk.joined ? std::format(" \xc2\xb7 blend {:.2f} s{}", walk.blend / 1000.0f,
                                              !distance                                             ? ""
                                              : TransitionLogic::IsCleanJoin(*distance, walk.blend) ? " Smooth"
                                                                                                    : " Pops")
                                : std::string(),
                    walk.joined ? "; drag the ramp's edges for the blend" : "");
            }
            else if (first.kind == Kind::Clip || first.kind == Kind::FaceClip)
            {
                const auto& clips = first.kind == Kind::Clip ? actor->clips : actor->faceClips;
                const auto clip = std::ranges::find(clips, static_cast<std::uint32_t>(first.id), &Types::ActorClip::id);
                if (clip != clips.end())
                    text = std::format(
                        "{}{}\n{:.2f} s from {}{}{}\nDrag to move it, its ends to trim it; right-click for more",
                        clip->animation, first.kind == Kind::FaceClip ? " (the face)" : "", clip->length / 1000.0f,
                        time, clip->loop ? ", looping" : "",
                        clip->anchor == Types::ActorAnchor::Time
                            ? std::string()
                            : std::format("\n" ICON_FA_ANCHOR " Hangs on {}, {:+.2f} s: it goes with it as the path "
                                          "changes",
                                          clip->anchor == Types::ActorAnchor::End
                                              ? std::string("the path's end")
                                              : std::format("point {}", clip->anchorPoint + 1),
                                          clip->anchorOffset / 1000.0f));
            }
            else if (first.kind == Kind::Event)
            {
                const auto event =
                    std::ranges::find(actor->events, static_cast<std::uint32_t>(first.id), &Types::ActorEvent::id);
                const auto killstreak = Components::KillstreakLogic::FindByModels(actor->models);
                const auto drops = killstreak && Components::KillstreakLogic::Get(*killstreak).guns.empty();
                if (event != actor->events.end())
                    text = event->type == Types::ActorEventType::Fire
                               ? (drops ? std::format("Drops {} from {}; drag to move, right-click for more",
                                                      event->shots, time)
                                        : std::format("Fires {} shots from {}; drag to move, right-click for more",
                                                      event->shots, time))
                               : std::format("{} at {}; drag to move, right-click for more",
                                             DescribeEventName(event->type), time);
            }
            else
            {
                text = std::format("Key of {} at {} (tick {}); drag to move, right-click for more", actor->name, time,
                                   item.start);
            }
        }
        if (!text.empty())
            ImGui::SetTooltip("%s", text.c_str());
    }

    void TimelineFrame::HandleInput()
    {
        auto& io = ImGui::GetIO();
        const auto mouse = io.MousePos;

        // what is under the mouse: the item drawn last there, else a lane
        std::optional<std::size_t> hovered;
        std::optional<std::size_t> hoveredLane;
        if (drag.gesture == Gesture::None)
        {
            for (auto i = drawn.size(); i-- > 0;)
            {
                const auto& item = drawn[i];
                if ((item.lane == MARKERS_ROW ? markersHovered : lanesHovered) && item.rect.Contains(mouse))
                {
                    hovered = i;
                    break;
                }
            }
            for (std::size_t i = 0; i < lanes.size() && lanesHovered; ++i)
            {
                if (lanes[i].rect.Contains(mouse))
                    hoveredLane = i;
            }
        }

        // the wheel: over the ruler and markers it zooms, over the lanes with Ctrl; with Shift it pans
        const bool overTimeline = rulerHovered || markersHovered || lanesHovered;
        if (io.MouseWheel != 0.0f && overTimeline && !ImGui::IsAnyItemActive())
        {
            if (io.KeyShift)
                timeline.SetView(TimelineMath::Pan(timeline.view,
                                                   -io.MouseWheel * 0.1 * (timeline.view.end - timeline.view.start),
                                                   static_cast<std::int32_t>(endTick)));
            else if (io.KeyCtrl || rulerHovered || markersHovered)
                timeline.SetView(TimelineMath::Zoom(timeline.view, io.MouseWheel, (mouse.x - axis.x0) / axis.width,
                                                    MIN_SPAN, static_cast<std::int32_t>(endTick)));
        }

        const auto rampEdge = drag.gesture == Gesture::None && lanesHovered ? RampEdgeAt(mouse) : std::nullopt;
        const auto* walkEnd = drag.gesture == Gesture::None && lanesHovered && !rampEdge ? WalkEndAt(mouse) : nullptr;
        if (rampEdge)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (!ImGui::IsAnyMouseDown())
                ImGui::SetTooltip("Blend %.2f s: drag either edge to change it (Alt+drag: Auto)",
                                  rampEdge->first->piece.blend / 1000.0f);
        }
        else if (walkEnd)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            const auto actor = Components::Actors::GetActor(walkEnd->actor);
            const auto timing = actor ? Components::Actors::GetPathTiming(*actor) : ActorLogic::PathTiming{};
            if (!ImGui::IsAnyMouseDown() && timing.arrival)
                ImGui::SetTooltip("Arrives at %.2f s: drag to change when (its pace follows)",
                                  ActorToTimeline(*timing.arrival) / 1000.0f);
        }
        else if (hovered)
        {
            const auto& item = drawn[*hovered];
            if ((item.trimStart && std::abs(mouse.x - item.startX) < EDGE_PIXELS) ||
                (item.trimEnd && std::abs(mouse.x - item.endX) < EDGE_PIXELS))
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (!ImGui::IsAnyMouseDown())
                DrawTooltip(item);
        }
        else if (markersHovered && drag.gesture == Gesture::None &&
                 std::ranges::any_of(spanHandles, [&](const SpanHandle& handle) { return handle.end.Contains(mouse); }))
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        else if (markersHovered && drag.gesture == Gesture::None &&
                 (rangeHandles.start.Contains(mouse) || rangeHandles.end.Contains(mouse)))
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SetTooltip("Drag to move the capture range's %s; drag the band to move it",
                              rangeHandles.start.Contains(mouse) ? "start" : "end");
        }
        else if (lanesHovered && drag.gesture == Gesture::None && OnPlayhead(mouse))
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        else if (rulerHovered && drag.gesture == Gesture::None && !ImGui::IsAnyMouseDown())
        {
            const auto tick = std::clamp<std::int64_t>(std::llround(axis.ToTick(mouse.x)), 0, endTick);
            ImGui::SetTooltip("%s (tick %lld); click or drag to go there", TimelineMath::FormatTime(tick).c_str(),
                              static_cast<long long>(tick));
        }

        if (overTimeline && drag.gesture == Gesture::None)
        {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            {
                drag = {Gesture::Pan, ImGuiMouseButton_Middle, mouse};
                drag.view = timeline.view;
            }
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                Press(hovered, hoveredLane);
            }
        }

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && overTimeline)
        {
            const auto tick =
                static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::llround(axis.ToTick(mouse.x)), 0, endTick));
            // (the press of a double-click's second click already picked what is under it)
            const auto under = [&]() -> std::optional<std::size_t> {
                for (auto i = drawn.size(); i-- > 0;)
                {
                    if ((drawn[i].lane == MARKERS_ROW ? markersHovered : lanesHovered) && drawn[i].rect.Contains(mouse))
                        return i;
                }
                return std::nullopt;
            }();
            std::optional<std::size_t> lane;
            for (std::size_t i = 0; i < lanes.size() && lanesHovered; ++i)
            {
                if (lanes[i].rect.Contains(mouse))
                    lane = i;
            }
            if (under)
            {
                const auto& item = drawn[*under];
                if (item.items.front().kind == Kind::Marker)
                {
                    renamedMarker = static_cast<std::uint32_t>(item.items.front().id);
                    const auto marker = Components::Markers::Find(renamedMarker);
                    std::snprintf(renameText, sizeof(renameText), "%s", marker ? marker->name.c_str() : "");
                    openRename = true;
                }
                else
                {
                    // (a walk's piece: its join's point)
                    JumpTo(static_cast<std::uint32_t>(item.jump.value_or(item.start)));
                }
                drag = {};
            }
            else if (const auto piece = PathPieceAt(mouse))
            {
                // a piece of a path bar: selected, and the playhead to when it sets off along it or gets there
                const auto at = GetPathPieceTick(*piece->first, piece->second);
                SelectPathPiece(piece->first->actor, piece->second);
                if (at)
                    JumpTo(ActorToTimeline(*at));
                drag = {};
            }
            else if (lane)
            {
                const auto& row = lanes[*lane];
                switch (row.kind)
                {
                    case RowKind::Campath:
                    case RowKind::Property:
                        AddKeyAt(
                            KeyframeManager::Get().GetProperty(static_cast<Types::KeyframeablePropertyType>(row.owner)),
                            tick);
                        break;
                    case RowKind::ActorEvents:
                    case RowKind::ActorClips:
                    case RowKind::ActorKeys:
                        if (const auto actor = Components::Actors::GetActor(row.owner); actor && !actor->mortar)
                        {
                            const Item item{
                                Kind::Clip, actor->id,
                                Components::Actors::AddClip(actor->id, MakeClipAt(*actor, TimelineToActor(tick)))};
                            TimelineSelection::Set(std::span(&item, 1));
                            ShowInPanel(item);
                        }
                        break;
                    case RowKind::ActorFace:
                        if (const auto actor = Components::Actors::GetActor(row.owner))
                        {
                            const Item item{Kind::FaceClip, actor->id,
                                            Components::Actors::AddFaceClip(
                                                actor->id, MakeFaceClipAt(*actor, TimelineToActor(tick)))};
                            TimelineSelection::Set(std::span(&item, 1));
                            ShowInPanel(item);
                        }
                        break;
                    case RowKind::Look:
                    case RowKind::Effect:
                    case RowKind::Light:
                    case RowKind::Group:
                        break;
                }
                drag = {};
            }
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && overTimeline && drag.gesture == Gesture::None)
        {
            menuTick =
                static_cast<std::uint32_t>(std::clamp<std::int64_t>(std::llround(axis.ToTick(mouse.x)), 0, endTick));
            if (hovered)
            {
                const auto& item = drawn[*hovered];
                if (!std::ranges::all_of(item.items, TimelineSelection::IsSelected))
                {
                    TimelineSelection::Set(item.items);
                    if (item.items.size() == 1)
                        ShowInPanel(item.items.front());
                }
                TimelineSelection::SetAnchor(item.items.front());
                menuTick = static_cast<std::uint32_t>(item.start);
                menuKind = MenuKind::Items;
                ImGui::OpenPopup(ITEM_MENU);
            }
            else if (const auto piece = PathPieceAt(mouse))
            {
                // a stretch or point of a path bar: selected, with the view's menu for it
                SelectPathPiece(piece->first->actor, piece->second);
                menuPathActor = piece->first->actor;
                menuPathPiece = piece->second;
                menuKind = MenuKind::Path;
                ImGui::OpenPopup(PATH_MENU);
            }
            else if (hoveredLane)
            {
                menuLane = lanes[*hoveredLane];
                menuKind = MenuKind::Lane;
                ImGui::OpenPopup(LANE_MENU);
            }
            else if (const auto span = std::ranges::find_if(
                         spanHandles,
                         [&](const SpanHandle& handle) { return handle.band.Contains(ImGui::GetMousePos()); });
                     markersHovered && span != spanHandles.end())
            {
                menuSpan = span->index;
                menuKind = MenuKind::Span;
                ImGui::OpenPopup(SPAN_MENU);
            }
            else if (rulerHovered || markersHovered)
            {
                menuKind = MenuKind::Ruler;
                ImGui::OpenPopup(RULER_MENU);
            }
        }

        UpdateDrag();
        if (drag.gesture != Gesture::None && !ImGui::IsMouseDown(drag.button))
            Release();
    }

    void TimelineFrame::Press(std::optional<std::size_t> hovered, std::optional<std::size_t> lane)
    {
        auto& io = ImGui::GetIO();
        const auto mouse = io.MousePos;
        drag = {};
        drag.press = mouse;
        drag.pressTick = axis.ToTick(mouse.x);
        drag.button = ImGuiMouseButton_Left;

        // Alt+drag on an empty lane pans, as the middle button does (in the ruler Alt only stops snapping)
        if (io.KeyAlt && !hovered && lane)
        {
            drag.gesture = Gesture::Pan;
            drag.view = timeline.view;
            return;
        }

        // an edge of a join's ramp on a walk's piece: its blend, the piece selected
        if (const auto edge = RampEdgeAt(mouse))
        {
            const auto stretch = edge->first->piece.first;
            const auto actor = Components::Actors::GetActor(edge->first->actor);
            const auto timing = actor ? Components::Actors::GetPathTiming(*actor) : ActorLogic::PathTiming{};
            if (actor && stretch < timing.reached.size() && timing.reached[stretch])
            {
                const Item item{Kind::Clip, actor->id, edge->first->piece.clip};
                TimelineSelection::Set(std::span(&item, 1));
                TimelineSelection::SetAnchor(item);
                ShowInPanel(item);
                SelectPiece(actor->id, stretch);
                drag.gesture = Gesture::JoinBlend;
                drag.actor = *actor;
                drag.joinStretch = stretch;
                drag.joinPoint = *timing.reached[stretch];
                return;
            }
        }

        // the end of an actor's path bar, or the right edge of its walk's last piece: when it arrives
        std::optional<std::uint32_t> arriving;
        if (const auto* walkEnd = WalkEndAt(mouse))
        {
            const auto actor = Components::Actors::GetActor(walkEnd->actor);
            if (actor && Components::Actors::GetPathTiming(*actor).arrival)
                arriving = walkEnd->actor;
        }
        for (const auto& handle : pathHandles)
            if (!arriving && lanesHovered && handle.end.Contains(mouse))
                arriving = handle.actor;
        if (arriving)
        {
            drag.gesture = Gesture::PathEnd;
            drag.pathActor = *arriving;
            drag.pathTick = std::llround(drag.pressTick);
            CollectSnapTargets(axis, {}, true);
            return;
        }

        // an end of an effect's or a light's bar trims it, selected (its middle is an item as any other, below)
        for (const auto& handle : effectHandles)
        {
            if (!lanesHovered)
                break;
            const int edge = handle.start.Contains(mouse) ? -1 : handle.end.Contains(mouse) ? 1 : 0;
            if (edge == 0)
                continue;
            const auto actor = Components::Actors::GetActor(handle.actor);
            const auto bar = actor ? ActorLogic::GetBar(*actor) : std::nullopt;
            // (a key on a light's bar is the key's; with Ctrl or Shift it is selected, as a clip's ends are)
            if (!bar || io.KeyMods != ImGuiMod_None ||
                (hovered && !TimelineSelection::IsBar(drawn[*hovered].items.front())))
                break;
            const auto item = TimelineSelection::BarItem(*actor);
            TimelineSelection::Set(std::span(&item, 1));
            TimelineSelection::SetAnchor(item);
            ShowInPanel(item);
            drag.gesture = Gesture::EffectBar;
            drag.effectActor = actor->id;
            drag.effectEdge = edge;
            drag.barStart = bar->first;
            drag.barEnd = bar->second;
            CollectSnapTargets(axis, {item}, true);
            return;
        }

        if (hovered)
        {
            const auto& item = drawn[*hovered];
            // a clip's ends trim it
            const bool onStart = item.trimStart && std::abs(mouse.x - item.startX) < EDGE_PIXELS;
            const bool onEnd = item.trimEnd && std::abs(mouse.x - item.endX) < EDGE_PIXELS;
            if ((onStart || onEnd) && io.KeyMods == ImGuiMod_None)
            {
                const auto& clipItem = item.items.front();
                if (const auto actor = Components::Actors::GetActor(clipItem.owner))
                {
                    const bool face = clipItem.kind == Kind::FaceClip;
                    const auto& clips = face ? actor->faceClips : actor->clips;
                    const auto clip =
                        std::ranges::find(clips, static_cast<std::uint32_t>(clipItem.id), &Types::ActorClip::id);
                    if (clip != clips.end())
                    {
                        TimelineSelection::Set(item.items);
                        TimelineSelection::SetAnchor(clipItem);
                        ShowInPanel(clipItem);
                        drag.gesture = Gesture::Trim;
                        drag.actor = *actor;
                        drag.clip = *clip;
                        drag.face = face;
                        // (the end when both are that close, as on a short clip)
                        drag.trimStart = !onEnd;
                        CollectSnapTargets(axis, {clipItem}, true);
                        return;
                    }
                }
            }

            drag.gesture = Gesture::PressItem;
            drag.items = item.items;
            const bool selected = std::ranges::all_of(item.items, TimelineSelection::IsSelected);
            if (io.KeyMods == ImGuiMod_Ctrl)
            {
                for (const auto& each : item.items)
                    TimelineSelection::Toggle(each);
            }
            else if (io.KeyMods == ImGuiMod_Shift)
            {
                SelectRange(item);
            }
            else if (!selected)
            {
                TimelineSelection::Set(item.items);
                if (item.items.size() == 1)
                    ShowInPanel(item.items.front());
            }
            else
            {
                drag.collapse = TimelineSelection::Get().size() > item.items.size();
                drag.showAgain = !drag.collapse;
            }
            TimelineSelection::SetAnchor(item.items.front());
            // (a walk's piece: its stretch too)
            if (item.stretch && io.KeyMods == ImGuiMod_None)
                SelectPiece(item.items.front().owner, *item.stretch);
            return;
        }

        if (markersHovered)
        {
            // a bullet time span's end changes how long it is
            for (const auto& handle : spanHandles)
            {
                if (handle.end.Contains(mouse))
                {
                    drag.gesture = Gesture::SpanEnd;
                    drag.span = handle.index;
                    Components::BulletTime::BeginResize(handle.index);
                    CollectSnapTargets(axis, {}, true);
                    return;
                }
            }

            // the capture range's ends, the band between them, else scrubbing
            const auto [start, end] = GetCaptureRange();
            drag.range = {start, end};
            if (rangeHandles.start.Contains(mouse) || rangeHandles.end.Contains(mouse))
            {
                drag.gesture = Gesture::Range;
                drag.rangeEnd = rangeHandles.end.Contains(mouse) ? 1 : -1;
                CollectSnapTargets(axis, {}, true, drag.rangeEnd != -1, drag.rangeEnd != 1);
                return;
            }
            if (rangeHandles.band.Contains(mouse))
            {
                drag.gesture = Gesture::Range;
                drag.rangeEnd = 0;
                CollectSnapTargets(axis, {}, true, false, false);
                return;
            }
        }

        // the ruler and the markers row scrub, and so does the playhead's line in the lanes and over the path bars
        const auto pathPiece = PathPieceAt(mouse);
        if (rulerHovered || markersHovered || ((lane || pathPiece) && OnPlayhead(mouse)))
        {
            drag.gesture = Gesture::Scrub;
            CollectSnapTargets(axis, {}, false);
            Seek(SnapTick(timeline, axis, std::llround(drag.pressTick)), false);
            return;
        }

        // a stretch, point's tick or wait of an actor's path bar: selected once let go, a box once it drags
        if (pathPiece)
        {
            drag.gesture = Gesture::PressPath;
            drag.pathActor = pathPiece->first->actor;
            drag.pathPiece = pathPiece->second;
            drag.additive = io.KeyCtrl || io.KeyShift;
            drag.before = TimelineSelection::Get();
            return;
        }

        if (lane)
        {
            drag.gesture = Gesture::PressLane;
            drag.additive = io.KeyCtrl || io.KeyShift;
            drag.before = TimelineSelection::Get();
        }
    }

    // Shift+click: the items of the clicked one's lane from the item clicked before to it
    void TimelineFrame::SelectRange(const Drawn& clicked)
    {
        const auto anchor = TimelineSelection::GetAnchor();
        std::optional<std::int64_t> from;
        for (const auto& item : drawn)
        {
            if (anchor && item.lane == clicked.lane && std::ranges::find(item.items, *anchor) != item.items.end())
                from = item.start;
        }
        if (!from)
        {
            TimelineSelection::Add(clicked.items);
            return;
        }
        std::vector<std::int64_t> ticks;
        std::vector<const Drawn*> inLane;
        for (const auto& item : drawn)
        {
            if (item.lane != clicked.lane)
                continue;
            ticks.push_back(item.start);
            inLane.push_back(&item);
        }
        for (const auto index : TimelineMath::InRange(ticks, *from, clicked.start))
            TimelineSelection::Add(inLane[index]->items);
    }

    void TimelineFrame::UpdateDrag()
    {
        if (drag.gesture == Gesture::None || !ImGui::IsMouseDown(drag.button))
            return;
        const auto mouse = ImGui::GetMousePos();
        const auto mouseTick = axis.ToTick(mouse.x);
        const auto moved =
            std::abs(mouse.x - drag.press.x) > DRAG_PIXELS || std::abs(mouse.y - drag.press.y) > DRAG_PIXELS;

        switch (drag.gesture)
        {
            case Gesture::PressItem:
                if (moved && TimelineSelection::Count() > 0)
                {
                    drag.gesture = Gesture::Move;
                    CollectSnapTargets(axis, TimelineSelection::Get(), true);
                    TimelineSelection::BeginMove();
                    UpdateDrag();
                }
                break;
            case Gesture::Move:
            {
                const auto spans = TimelineSelection::GetMovingSpans();
                const auto raw = std::llround(mouseTick - drag.pressTick);
                const auto move =
                    TimelineMath::SnapMove(spans, raw,
                                           IsSnappingNow(timeline) ? std::span<const std::int64_t>(snapTargets)
                                                                   : std::span<const std::int64_t>(),
                                           axis.TicksPer(SNAP_PIXELS), 0, endTick);
                drag.delta = move.delta;
                snappedTick.reset();
                if (move.target)
                {
                    snappedTick = snapTargets[*move.target];
                    snappedName = snapNames[*move.target];
                }
                TimelineSelection::UpdateMove(move.delta);
                ImGui::SetTooltip("%s%s", DescribeSeconds(move.delta).c_str(),
                                  move.target ? std::format(", on {}", snappedName).c_str() : "");
                break;
            }
            case Gesture::Trim:
            {
                const auto delta = std::llround(mouseTick - drag.pressTick);
                const auto edge = TimelineToActor(SnapTick(
                    timeline, axis,
                    static_cast<std::int64_t>(ActorToTimeline(drag.trimStart ? drag.clip.start : drag.clip.End())) +
                        delta));
                auto edited = drag.actor;
                auto& clips = drag.face ? edited.faceClips : edited.clips;
                const auto clip = std::ranges::find(clips, drag.clip.id, &Types::ActorClip::id);
                if (clip == clips.end())
                    break;
                *clip = drag.trimStart ? ActorLogic::TrimClipStart(drag.clip, edge, MIN_CLIP_LENGTH)
                                       : ActorLogic::TrimClipEnd(drag.clip, edge, MIN_CLIP_LENGTH);
                Components::Actors::Update(edited, false, "trim clip of " + drag.actor.name);
                ImGui::SetTooltip(
                    "%s %s, %.2f s long", drag.trimStart ? "Starts at" : "Ends at",
                    TimelineMath::FormatTime(ActorToTimeline(drag.trimStart ? clip->start : clip->End())).c_str(),
                    clip->length / 1000.0f);
                break;
            }
            case Gesture::PressLane:
                if (moved)
                {
                    drag.gesture = Gesture::Box;
                    UpdateDrag();
                }
                break;
            case Gesture::PressPath:
            {
                // dragged: the walk slides, the tick nearest where it was grabbed
                // (where it sets off, a point's, the end) snapping to the playhead, markers and the rest
                const auto actor = moved ? Components::Actors::GetActor(drag.pathActor) : std::nullopt;
                const auto timing = actor ? Components::Actors::GetPathTiming(*actor) : ActorLogic::PathTiming{};
                if (!actor || !timing.start)
                    break;
                std::vector<std::pair<std::uint32_t, std::string>> grabs{{*timing.start, "its set-off"}};
                for (std::size_t i = 1; i + 1 < timing.reached.size(); ++i)
                    if (timing.reached[i])
                        grabs.emplace_back(*timing.reached[i], std::format("point {}", i));
                if (timing.arrival)
                    grabs.emplace_back(*timing.arrival, "its end");
                const auto nearest = std::ranges::min_element(grabs, {}, [&](const auto& grab) {
                    return std::abs(static_cast<double>(ActorToTimeline(grab.first)) - drag.pressTick);
                });
                drag.actor = *actor;
                drag.slideSetOff = *timing.start;
                drag.slideGrab = ActorToTimeline(nearest->first);
                drag.slideGrabName = nearest->second;
                CollectSnapTargets(axis, {}, true, true, true, actor->id);
                drag.gesture = Gesture::SlidePath;
                UpdateDrag();
                break;
            }
            case Gesture::SlidePath:
            {
                const std::array spans{TimelineMath::Span{drag.slideGrab, drag.slideGrab}};
                const auto move =
                    TimelineMath::SnapMove(spans, std::llround(mouseTick - drag.pressTick),
                                           IsSnappingNow(timeline) ? std::span<const std::int64_t>(snapTargets)
                                                                   : std::span<const std::int64_t>(),
                                           axis.TicksPer(SNAP_PIXELS), 0, endTick);
                snappedTick.reset();
                if (move.target)
                {
                    snappedTick = snapTargets[*move.target];
                    snappedName = snapNames[*move.target];
                }
                // (in the actor's ticks: bullet time puts the timeline's later)
                const auto delta = static_cast<std::int64_t>(TimelineToActor(drag.slideGrab + move.delta)) -
                                   static_cast<std::int64_t>(TimelineToActor(drag.slideGrab));
                auto edited = drag.actor;
                const auto slid = ActorLogic::SlideWalk(edited, drag.slideSetOff, delta);
                Components::Actors::Update(edited, false, "slide the walk of " + drag.actor.name);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                ImGui::SetTooltip(
                    "%s%s", DescribeSeconds(slid).c_str(),
                    move.target ? std::format(", {} on {}", drag.slideGrabName, snappedName).c_str() : "");
                break;
            }
            case Gesture::JoinBlend:
            {
                // as far from the point as the mouse (twice, centred on it); Alt: Auto
                if (drag.joinStretch >= drag.actor.stretches.size())
                    break;
                auto edited = drag.actor;
                auto& stretch = edited.stretches[drag.joinStretch];
                ActorLogic::UpdateJoin(stretch);
                const auto off = std::abs(static_cast<std::int64_t>(TimelineToActor(std::llround(mouseTick))) -
                                          static_cast<std::int64_t>(drag.joinPoint));
                const auto blend =
                    std::clamp<std::int64_t>((stretch.blendAt == Types::ActorJoinAt::Centred ? 2 : 1) * off,
                                             MIN_DRAGGED_BLEND, MAX_DRAGGED_BLEND);
                const bool automatic = ImGui::GetIO().KeyAlt;
                if (automatic)
                    stretch.blend.reset();
                else
                    stretch.blend = static_cast<std::uint32_t>((blend + 5) / 10 * 10);
                Components::Actors::Update(
                    edited, false,
                    std::format("change the blend at point {} of {}", drag.joinStretch, drag.actor.name));
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                ImGui::SetTooltip(
                    "Blend %.2f s%s",
                    ActorLogic::GetJoinBlend(edited, drag.joinStretch, Components::Actors::GetAnimationLookup()) /
                        1000.0f,
                    automatic ? " (Auto)" : "");
                break;
            }
            case Gesture::Box:
            {
                std::vector<TimelineMath::Box> boxes;
                std::vector<const Drawn*> items;
                for (const auto& item : drawn)
                {
                    if (item.lane == MARKERS_ROW)
                        continue;
                    boxes.push_back({std::llround(axis.ToTick(item.rect.Min.x)),
                                     std::llround(axis.ToTick(item.rect.Max.x)), item.rect.Min.y, item.rect.Max.y});
                    items.push_back(&item);
                }
                std::set<Item> selection = drag.additive ? drag.before : std::set<Item>();
                // (it ends at the lanes' edges, as it is drawn: rows scrolled out of sight aren't taken)
                const auto bottom = std::clamp(mouse.y, lanesClip.Min.y, lanesClip.Max.y);
                for (const auto index : TimelineMath::InBox(
                         boxes, {std::llround(drag.pressTick), std::llround(mouseTick), drag.press.y, bottom}))
                    selection.insert(items[index]->items.begin(), items[index]->items.end());
                TimelineSelection::Set(std::vector<Item>(selection.begin(), selection.end()));
                break;
            }
            case Gesture::Scrub:
                if (moved)
                {
                    const auto tick = SnapTick(timeline, axis, std::llround(mouseTick));
                    Seek(tick, true);
                    ImGui::SetTooltip("%s%s",
                                      TimelineMath::FormatTime(std::clamp<std::int64_t>(tick, 0, endTick)).c_str(),
                                      snappedTick ? std::format(", on {}", snappedName).c_str() : "");
                }
                break;
            case Gesture::EffectBar:
            {
                auto actor = Components::Actors::GetActor(drag.effectActor);
                if (!actor || (!actor->effect && !actor->light && !actor->placedProp) || !moved)
                    break;
                const auto delta = std::llround(mouseTick - drag.pressTick);
                // the edge dragged, snapping as clips do
                const auto moving =
                    static_cast<std::int64_t>(ActorToTimeline(drag.effectEdge > 0 ? drag.barEnd : drag.barStart));
                const auto to = static_cast<std::int64_t>(TimelineToActor(
                    std::clamp<std::int64_t>(SnapTick(timeline, axis, moving + delta), 0, endTick)));
                auto& start = actor->effect  ? actor->effect->start
                              : actor->light ? actor->light->start
                                             : actor->placedProp->start;
                auto& end = actor->effect  ? actor->effect->end
                            : actor->light ? actor->light->end
                                           : actor->placedProp->end;
                if (drag.effectEdge < 0)
                    start = static_cast<std::uint32_t>(std::min<std::int64_t>(to, drag.barEnd - 100));
                else
                    end = static_cast<std::uint32_t>(std::max<std::int64_t>(to, drag.barStart + 100));
                Components::Actors::Update(*actor, false, "trim " + actor->name);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                ImGui::SetTooltip("%s to %s%s", TimelineMath::FormatTime(ActorToTimeline(start)).c_str(),
                                  TimelineMath::FormatTime(ActorToTimeline(end)).c_str(),
                                  snappedTick ? std::format(", on {}", snappedName).c_str() : "");
                break;
            }
            case Gesture::PathEnd:
            {
                drag.pathMoved = drag.pathMoved || moved;
                drag.pathTick = std::max<std::int64_t>(SnapTick(timeline, axis, std::llround(mouseTick)), 0);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                const auto actor = Components::Actors::GetActor(drag.pathActor);
                const auto timing = actor ? Components::Actors::GetPathTiming(*actor) : ActorLogic::PathTiming{};
                const auto gait = actor ? Components::Actors::GetGait(*actor, 0) : std::nullopt;
                if (timing.start && timing.arrival && gait)
                {
                    // (as a guess from how long it goes now; worked out once let go)
                    std::uint32_t waits = 0;
                    for (std::size_t i = 0; i + 1 < actor->stretches.size(); ++i)
                        waits += actor->stretches[i].wait;
                    const auto target = static_cast<float>(TimelineToActor(drag.pathTick));
                    const auto moving = static_cast<float>(*timing.arrival - *timing.start) - static_cast<float>(waits);
                    const auto wanted = target - static_cast<float>(*timing.start) - static_cast<float>(waits);
                    const auto pace = wanted > 100.0f ? gait->pace * moving / wanted : 0.0f;
                    const auto inRange = pace >= ActorLogic::MIN_PACE && pace <= ActorLogic::MAX_PACE;
                    ImGui::SetTooltip("Arrive %s · %s %.0f%%%s%s", TimelineMath::FormatTime(drag.pathTick).c_str(),
                                      ActorActions::GetGaitName(*actor, gait->gait, gait->crouched).c_str(),
                                      pace * 100.0f,
                                      inRange       ? ""
                                      : pace > 1.0f ? " · too fast"
                                                    : " · too slow",
                                      snappedTick ? std::format(", on {}", snappedName).c_str() : "");
                }
                break;
            }
            case Gesture::SpanEnd:
            {
                const auto spans = Components::Playback::GetFrozenSpans();
                if (drag.span >= spans->size())
                    break;
                const auto start = static_cast<std::int64_t>(Components::FrozenSpans::GetStart(*spans, drag.span));
                const auto end = SnapTick(timeline, axis, std::llround(mouseTick));
                const auto length =
                    static_cast<std::uint32_t>(std::max<std::int64_t>(end - start, Components::BulletTime::MIN_LENGTH));
                Components::BulletTime::UpdateResize(length);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                ImGui::SetTooltip("Bullet time %.2f s%s", length / 1000.0,
                                  snappedTick ? std::format(", on {}", snappedName).c_str() : "");
                break;
            }
            case Gesture::Pan:
                timeline.SetView(TimelineMath::Pan(drag.view, -axis.TicksPer(mouse.x - drag.press.x),
                                                   static_cast<std::int32_t>(endTick)));
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                break;
            case Gesture::Range:
            {
                auto& settings = Components::CaptureManager::Get().GetCaptureSettings();
                const auto delta = std::llround(mouseTick - drag.pressTick);
                const auto [start, end] = drag.range;
                if (drag.rangeEnd == 0)
                {
                    const std::array span{TimelineMath::Span{start, end}};
                    const auto length = static_cast<std::int64_t>(end) - start;
                    const auto move = TimelineMath::SnapMove(
                        span, delta,
                        IsSnappingNow(timeline) ? std::span<const std::int64_t>(snapTargets)
                                                : std::span<const std::int64_t>(),
                        axis.TicksPer(SNAP_PIXELS), 0, std::max<std::int64_t>(endTick - length, 0));
                    snappedTick.reset();
                    if (move.target)
                    {
                        snappedTick = snapTargets[*move.target];
                        snappedName = snapNames[*move.target];
                    }
                    settings.startTick = static_cast<std::uint32_t>(start + move.delta);
                    settings.endTick = static_cast<std::uint32_t>(end + move.delta);
                }
                else
                {
                    const auto moving = drag.rangeEnd < 0 ? start : end;
                    const auto other = drag.rangeEnd < 0 ? end : start;
                    const auto tick = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
                        SnapTick(timeline, axis, static_cast<std::int64_t>(moving) + delta), 0, endTick));
                    // (dragged past the other end, they swap)
                    settings.startTick = std::min(tick, other);
                    settings.endTick = std::max(tick, other);
                }
                ImGui::SetTooltip("Capture range %s to %s%s", TimelineMath::FormatTime(settings.startTick).c_str(),
                                  TimelineMath::FormatTime(settings.endTick).c_str(),
                                  snappedTick ? std::format(", on {}", snappedName).c_str() : "");
                break;
            }
            case Gesture::None:
                break;
        }
    }

    void TimelineFrame::Release()
    {
        switch (drag.gesture)
        {
            case Gesture::PressItem:
                // a click on one of several selected items selects only it
                if (drag.collapse)
                {
                    TimelineSelection::Set(drag.items);
                    if (drag.items.size() == 1)
                        ShowInPanel(drag.items.front());
                }
                else if (drag.showAgain)
                {
                    ShowAgain(drag.items);
                }
                break;
            case Gesture::Move:
            {
                const auto snapped = snappedTick;
                const auto name = snappedName;
                TimelineSelection::EndMove();
                if (snapped && (name == PATH_END_SNAP || name == PATH_POINT_SNAP))
                    AnchorDropped(*snapped);
                break;
            }
            case Gesture::PressLane:
                if (!drag.additive)
                    TimelineSelection::Clear();
                break;
            case Gesture::PressPath:
                SelectPathPiece(drag.pathActor, drag.pathPiece);
                break;
            case Gesture::SpanEnd:
                Components::BulletTime::EndResize();
                break;
            case Gesture::PathEnd:
                // a click: its last point
                if (!drag.pathMoved)
                {
                    if (const auto actor = Components::Actors::GetActor(drag.pathActor); actor && !actor->path.empty())
                        SelectPathPiece(actor->id, {TimelineMath::PathPiece::Kind::End, actor->path.size() - 1});
                }
                else if (auto actor = Components::Actors::GetActor(drag.pathActor))
                {
                    const auto tick = TimelineToActor(drag.pathTick);
                    const auto arrival = Components::Actors::SolveArrival(*actor, tick);
                    const bool reached = Components::Actors::ArriveAt(*actor, tick);
                    Components::Actors::Update(*actor, false, "change when " + actor->name + " arrives");
                    // too fast or slow for its gait: the next gait, a click away (as the panel's Arrive at offers)
                    const auto gait = Components::Actors::GetGait(*actor, 0);
                    if (!reached && arrival && arrival->instead && gait)
                    {
                        const auto id = actor->id;
                        const auto instead = *arrival->instead;
                        const auto crouched = gait->crouched;
                        Toasts::Show(
                            std::format("{}: too {} for {}", actor->name, instead > gait->gait ? "fast" : "slow",
                                        ActorActions::GetGaitName(*actor, gait->gait, crouched)),
                            ActorActions::GetGaitName(*actor, instead, crouched) + "?", [id, instead, crouched, tick] {
                                auto actor = Components::Actors::GetActor(id);
                                if (!actor)
                                    return;
                                Components::Actors::SetGait(*actor, 0, actor->path.size(), instead, crouched);
                                Components::Actors::ArriveAt(*actor, tick);
                                Components::Actors::Update(*actor, false, "change how " + actor->name + " goes");
                            });
                    }
                    else if (!reached)
                        Toasts::Show(actor->name + " can't get there then at its gait: try another gait");
                }
                break;
            case Gesture::Range:
            {
                const auto range = GetCaptureRange();
                PushCaptureRange(drag.range, range,
                                 drag.rangeEnd == 0  ? "move the capture range"
                                 : drag.rangeEnd < 0 ? "set the capture start"
                                                     : "set the capture end");
                break;
            }
            default:
                break;
        }
        snappedTick.reset();
        drag = {};
    }

    void TimelineFrame::DrawMenus()
    {
        const auto shortcut = [](std::string_view id) { return Commands::GetShortcut(id); };

        if (ImGui::BeginPopup(ITEM_MENU))
        {
            const std::vector<Item> items(TimelineSelection::Get().begin(), TimelineSelection::Get().end());
            if (items.empty())
            {
                ImGui::CloseCurrentPopup();
            }
            else
            {
                ImGui::TextDisabled("%s at %s", TimelineSelection::Describe(items).c_str(),
                                    TimelineMath::FormatTime(menuTick).c_str());
                if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Go to"))
                    JumpTo(menuTick);
                const auto& only = items.front();
                if (items.size() == 1 && only.kind == Kind::Key &&
                    only.owner == static_cast<std::uint32_t>(Types::KeyframeablePropertyType::CampathCamera))
                {
                    const auto reason = Commands::GetUnavailableReason("campath.updateNode");
                    if (ImGui::MenuItem(ICON_FA_CAMERA_ROTATE " Set from the view",
                                        shortcut("campath.updateNode").c_str(), false, !reason))
                        Commands::Run("campath.updateNode");
                    if (reason)
                        ImGui::SetItemTooltip("%s", reason->c_str());
                }
                if (items.size() == 1 && only.kind == Kind::Marker && ImGui::MenuItem(ICON_FA_PEN " Rename..."))
                {
                    renamedMarker = static_cast<std::uint32_t>(only.id);
                    const auto marker = Components::Markers::Find(renamedMarker);
                    std::snprintf(renameText, sizeof(renameText), "%s", marker ? marker->name.c_str() : "");
                    openRename = true;
                }
                if (items.size() == 1 && (only.kind == Kind::Clip || only.kind == Kind::FaceClip))
                {
                    const bool face = only.kind == Kind::FaceClip;
                    if (auto actor = Components::Actors::GetActor(only.owner))
                    {
                        auto& clips = face ? actor->faceClips : actor->clips;
                        const auto clip =
                            std::ranges::find(clips, static_cast<std::uint32_t>(only.id), &Types::ActorClip::id);
                        if (clip != clips.end())
                        {
                            const auto actorTick = TimelineToActor(currentTick);
                            if (ImGui::MenuItem(ICON_FA_SCISSORS " Split at the playhead", nullptr, false,
                                                actorTick > clip->start && actorTick < clip->End()))
                            {
                                auto second = *clip;
                                second.id = 0;
                                second.start = actorTick;
                                second.length = clip->End() - actorTick;
                                second.offset = clip->offset + (actorTick - clip->start) / 1000.0f * clip->speed;
                                second.blendIn = 0;
                                clip->length = actorTick - clip->start;
                                Components::Actors::Update(*actor, false, "split clip of " + actor->name);
                                (face ? Components::Actors::AddFaceClip : Components::Actors::AddClip)(actor->id,
                                                                                                       second);
                            }
                            if (ImGui::MenuItem(ICON_FA_CLONE " Duplicate after"))
                            {
                                auto copy = *clip;
                                copy.start = clip->End();
                                (face ? Components::Actors::AddFaceClip : Components::Actors::AddClip)(actor->id, copy);
                            }
                        }
                    }
                }
                DrawKeyOptions();
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_CLONE " Duplicate", shortcut("edit.duplicate").c_str()))
                    TimelineSelection::Duplicate();
                if (ImGui::MenuItem(ICON_FA_COPY " Copy", shortcut("edit.copy").c_str()))
                    TimelineSelection::Copy();
                if (ImGui::MenuItem(ICON_FA_PASTE " Paste here", shortcut("edit.paste").c_str(), false,
                                    TimelineSelection::CountCopied() > 0))
                    TimelineSelection::Paste(menuTick);
                if (ImGui::MenuItem(ICON_FA_TRASH " Delete", shortcut("edit.delete").c_str()))
                    TimelineSelection::Delete();
            }
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup(PATH_MENU))
        {
            // a stretch: Insert point here (where the actor is at the time clicked) and its gait; a point: as the view
            const auto actor = Components::Actors::GetActor(menuPathActor);
            if (!actor)
                ImGui::CloseCurrentPopup();
            else if (menuPathPiece.kind == TimelineMath::PathPiece::Kind::Stretch)
                ViewMenu::DrawPathLineMenu(*actor, menuPathPiece.index, TimelineToActor(menuTick), "timeline.path.");
            else
                ViewMenu::DrawPathPointMenu(*actor, menuPathPiece.index, "timeline.path.");
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup(LANE_MENU))
        {
            ImGui::TextDisabled("At %s", TimelineMath::FormatTime(menuTick).c_str());
            switch (menuLane.kind)
            {
                case RowKind::Campath:
                case RowKind::Property:
                {
                    const auto& property = KeyframeManager::Get().GetProperty(
                        static_cast<Types::KeyframeablePropertyType>(menuLane.owner));
                    const bool campath = property.type == Types::KeyframeablePropertyType::CampathCamera;
                    if (ImGui::MenuItem(campath ? ICON_FA_DIAMOND " Add a node here"
                                                : ICON_FA_DIAMOND " Add a key here"))
                        AddKeyAt(property, menuTick);
                    if (ImGui::MenuItem(ICON_FA_CHART_LINE " In the graph", nullptr,
                                        TimelineGraph::Shows(property.type)))
                        TimelineGraph::Toggle(property.type);
                    AddRow("menu.graph");
                    if (campath)
                    {
                        if (ImGui::MenuItem(ICON_FA_GAUGE " Constant speed", shortcut("campath.constantSpeed").c_str(),
                                            KeyframeManager::Get().IsConstantSpeed()))
                            Commands::Run("campath.constantSpeed");
                        AddRow("menu.constantspeed");
                        ImGui::SetItemTooltip(
                            "The camera goes along the path at one speed from the first node to "
                            "the last, whenever the nodes between are");
                    }
                    break;
                }
                case RowKind::ActorEvents:
                case RowKind::ActorClips:
                case RowKind::ActorKeys:
                {
                    const auto actor = Components::Actors::GetActor(menuLane.owner);
                    if (!actor)
                        break;
                    const auto actorTick = TimelineToActor(menuTick);
                    const auto opened = [&](Item item) {
                        TimelineSelection::Set(std::span(&item, 1));
                        ShowInPanel(item);
                    };
                    if (!actor->mortar && ImGui::MenuItem(ICON_FA_FILM " Clip"))
                        opened({Kind::Clip, actor->id,
                                Components::Actors::AddClip(actor->id, MakeClipAt(*actor, actorTick))});
                    ImGui::Separator();
                    for (const auto type :
                         {Types::ActorEventType::Fire, Types::ActorEventType::Hit, Types::ActorEventType::Effect,
                          Types::ActorEventType::Show, Types::ActorEventType::Hide, Types::ActorEventType::Look})
                    {
                        // (the Mortar Team has no body: its Fire, Show and Hide alone)
                        const bool bodily = type == Types::ActorEventType::Hit ||
                                            type == Types::ActorEventType::Effect ||
                                            type == Types::ActorEventType::Look;
                        const auto unavailable =
                            type == Types::ActorEventType::Fire ? ActorActions::GetFireUnavailableReason(*actor)
                            : actor->mortar && bodily ? std::optional<std::string>("The Mortar Team has no body")
                                                      : std::nullopt;
                        if (ImGui::MenuItem(
                                std::format("{} {}", DescribeEventIcon(type), DescribeEventName(type)).c_str(), nullptr,
                                false, !unavailable))
                        {
                            Types::ActorEvent event{.type = type, .tick = actorTick};
                            if (type == Types::ActorEventType::Fire)
                                event = actor->mortar ? Components::KillstreakLogic::MakeMortarFire(actorTick)
                                                      : Components::Actors::MakeGunFire(*actor, actorTick);
                            if (type == Types::ActorEventType::Effect)
                                event.tag = "j_spine4";
                            // (a killstreak's Fire as the game's, at the tick clicked: see MakeFire)
                            const auto killstreak = type == Types::ActorEventType::Fire
                                                        ? Components::KillstreakLogic::FindByModels(actor->models)
                                                        : std::nullopt;
                            if (killstreak)
                            {
                                const auto& info = Components::KillstreakLogic::Get(*killstreak);
                                if (info.guns.empty() &&
                                    info.fireDrop == Components::KillstreakLogic::FireDrop::Detonate)
                                    ActorActions::AddKillstreakFire(actor->id);
                                else
                                    opened({Kind::Event, actor->id,
                                            Components::Actors::AddEvent(
                                                actor->id,
                                                Components::KillstreakLogic::MakeFire(info, *actor, actorTick))});
                            }
                            else
                                opened({Kind::Event, actor->id, Components::Actors::AddEvent(actor->id, event)});
                        }
                        if (type == Types::ActorEventType::Fire)
                            AddRow("menu.fire");
                        if (unavailable)
                            ImGui::SetItemTooltip("%s", unavailable->c_str());
                    }
                    if (actor->movement == Types::ActorMovement::Keyframes)
                    {
                        ImGui::Separator();
                        if (ImGui::MenuItem(ICON_FA_DIAMOND " Key"))
                        {
                            // where it stands there now
                            auto edited = *actor;
                            const auto placement = Components::Actors::EvaluatePlacement(edited, actorTick);
                            Components::Actors::ApplyPlacement(edited, actorTick, placement.position, placement.yaw);
                            Components::Actors::Update(edited);
                            opened({Kind::ActorKey, actor->id, actorTick});
                        }
                    }
                    break;
                }
                case RowKind::ActorFace:
                {
                    auto actor = Components::Actors::GetActor(menuLane.owner);
                    if (!actor)
                        break;
                    if (ImGui::MenuItem(ICON_FA_FACE_SMILE " Face clip"))
                    {
                        const Item item{Kind::FaceClip, actor->id,
                                        Components::Actors::AddFaceClip(
                                            actor->id, MakeFaceClipAt(*actor, TimelineToActor(menuTick)))};
                        TimelineSelection::Set(std::span(&item, 1));
                        ShowInPanel(item);
                    }
                    AddRow("menu.faceclip");
                    if (ImGui::MenuItem("Its own face", nullptr, actor->face == Types::ActorFace::Auto))
                    {
                        actor->face =
                            actor->face == Types::ActorFace::Auto ? Types::ActorFace::Off : Types::ActorFace::Auto;
                        Components::Actors::Update(*actor);
                    }
                    AddRow("menu.face");
                    ImGui::SetItemTooltip("The face moves as the game's players' faces do (the Look page's Face)");
                    break;
                }
                case RowKind::Look:
                case RowKind::Effect:
                case RowKind::Light:
                case RowKind::Group:
                    break;
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_PASTE " Paste here", shortcut("edit.paste").c_str(), false,
                                TimelineSelection::CountCopied() > 0))
                TimelineSelection::Paste(menuTick);
            if (ImGui::MenuItem(ICON_FA_FLAG " Add marker"))
                AddMarkerAt(menuTick);
            if (ImGui::MenuItem(ICON_FA_SNOWFLAKE " Bullet time"))
                AddBulletTime(menuTick);
            AddRow("menu.bullettime");
            ImGui::SetItemTooltip("The demo stands still here for %.0f s while the camera and the Look keys go on",
                                  Components::BulletTime::DEFAULT_LENGTH / 1000.0);
            if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Go here"))
                JumpTo(menuTick);
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup(RULER_MENU))
        {
            ImGui::TextDisabled("At %s", TimelineMath::FormatTime(menuTick).c_str());
            if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Go here"))
                Seek(menuTick, false);
            if (ImGui::MenuItem("Range starts here", shortcut("capture.setStart").c_str()))
                SetCaptureRangeEnd(true, menuTick);
            if (ImGui::MenuItem("Range ends here", shortcut("capture.setEnd").c_str()))
                SetCaptureRangeEnd(false, menuTick);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_FLAG " Add marker", shortcut("timeline.addMarker").c_str()))
                AddMarkerAt(menuTick);
            if (ImGui::MenuItem(ICON_FA_SNOWFLAKE " Bullet time", shortcut("playback.bulletTime").c_str()))
                AddBulletTime(menuTick);
            AddRow("menu.bullettime");
            ImGui::SetItemTooltip("The demo stands still here for %.0f s while the camera and the Look keys go on",
                                  Components::BulletTime::DEFAULT_LENGTH / 1000.0);
            if (ImGui::MenuItem(ICON_FA_PASTE " Paste here", shortcut("edit.paste").c_str(), false,
                                TimelineSelection::CountCopied() > 0))
                TimelineSelection::Paste(menuTick);
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup(SPAN_MENU))
        {
            const auto spans = Components::Playback::GetFrozenSpans();
            if (menuSpan < spans->size())
            {
                const auto span = (*spans)[menuSpan];
                ImGui::TextDisabled("%s", Components::BulletTime::Describe(menuSpan).c_str());
                float seconds = span.length / 1000.0f;
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
                if (ImGui::InputFloat("Length (s)", &seconds, 0.1f, 1.0f, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue))
                    Components::BulletTime::SetLength(
                        menuSpan, static_cast<std::uint32_t>(std::lround(std::max(seconds, 0.1f) * 1000.0f)));
                AddRow("menu.span.length");
                bool move = span.actorsMove;
                if (ImGui::MenuItem("Actors keep moving", nullptr, &move))
                    Components::BulletTime::SetActorsMove(menuSpan, move);
                AddRow("menu.span.actors");
                ImGui::SetItemTooltip(
                    "Actors go on through it while the demo stands still (after it they are ahead of "
                    "the demo by its length); off, they stand still with it");
                if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Go to its start"))
                    Seek(Components::FrozenSpans::GetStart(*spans, menuSpan), false);
                AddRow("menu.span.goto");
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH " Remove bullet time"))
                    Components::BulletTime::Remove(menuSpan);
                AddRow("menu.span.remove");
            }
            ImGui::EndPopup();
        }

        if (openRename)
        {
            openRename = false;
            ImGui::OpenPopup(RENAME_POPUP);
        }
        if (ImGui::BeginPopup(RENAME_POPUP))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::InputText("##name", renameText, sizeof(renameText), ImGuiInputTextFlags_EnterReturnsTrue))
            {
                Components::Markers::Rename(renamedMarker, renameText);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void TimelineFrame::DrawKeyOptions()
    {
        const auto options = TimelineSelection::GetKeyOptions();
        if (options.keys == 0)
            return;
        ImGui::Separator();
        ImGui::TextDisabled(
            "%s", options.keys == 1 ? "Interpolation to the next key" : "Interpolation of the keys to the next");
        const auto item = [&](Types::KeyInterpolation interpolation, const char* label, const char* row,
                              const char* tooltip) {
            if (ImGui::MenuItem(label, nullptr, options.interpolation == interpolation))
                TimelineSelection::SetInterpolation(interpolation);
            AddRow(row);
            ImGui::SetItemTooltip("%s", tooltip);
        };
        const auto campath = static_cast<std::uint32_t>(Types::KeyframeablePropertyType::CampathCamera);
        const bool anyNode = std::ranges::any_of(TimelineSelection::Get(), [campath](const Item& each) {
            return each.kind == Kind::Key && each.owner == campath;
        });
        item(Types::KeyInterpolation::Smooth, "Smooth", "menu.smooth",
             "A curve through the keys around it, which doesn't overshoot them");
        item(Types::KeyInterpolation::Linear, "Linear", "menu.linear", "Straight to the next key");
        item(Types::KeyInterpolation::Hold, "Hold", "menu.hold", "Stays until the next key");
        // (for keys that have it, and campath nodes, which have it when they are added)
        if (options.anyAuto || anyNode)
            item(Types::KeyInterpolation::Auto, "Auto", "menu.auto",
                 "As keys always went, and new campath nodes do: straight while the track has under four "
                 "keys, a curve through all of them from four");
        // how much they ease, as one step a drag (with a message as it ends)
        const auto described = TimelineSelection::Describe(
            std::vector<Item>(TimelineSelection::Get().begin(), TimelineSelection::Get().end()));
        const auto sliderWidth = ImGui::GetFontSize() * 7.0f;
        const auto addRow = [this](const std::string& name) { AddRow(name); };
        for (const bool in : {true, false})
        {
            ImGuiEx::Keyframeable::Ease(
                in ? "Ease in" : "Ease out", in ? options.easeIn : options.easeOut, sliderWidth, 0.0f,
                std::format("change the ease {} of {}{}", in ? "in" : "out", options.keys == 1 ? "the " : "",
                            described),
                [in](float changed, bool ended) { TimelineSelection::SetEase(in, changed, ended); },
                in ? "Slow down to rest arriving at the key" : "Start from rest leaving the key",
                in ? "menu.easein" : "menu.easeout", addRow);
        }

        if (anyNode)
        {
            if (ImGui::MenuItem(ICON_FA_GAUGE " Constant speed", Commands::GetShortcut("campath.constantSpeed").c_str(),
                                KeyframeManager::Get().IsConstantSpeed()))
                Commands::Run("campath.constantSpeed");
            AddRow("menu.constantspeed");
            ImGui::SetItemTooltip(
                "The camera goes along the path at one speed from the first node to the last, "
                "whenever the nodes between are");
        }
    }

    void Timeline::SetView(TimelineMath::View shown)
    {
        const auto end = static_cast<std::int32_t>(GetEndTick());
        if (end <= 0)
        {
            view = shown;
            return;
        }
        shown.start = std::clamp(shown.start, 0, end);
        shown.end = std::clamp(shown.end, shown.start, end);
        if (shown.end - shown.start < std::min(MIN_SPAN, end))
            shown = TimelineMath::Fit(shown.start, shown.end, 0.0, MIN_SPAN, end);
        view = shown;
    }

    void Timeline::FitDemo()
    {
        SetView({0, static_cast<std::int32_t>(GetEndTick())});
    }

    void Timeline::FitCaptureRange()
    {
        const auto [start, end] = GetCaptureRange();
        SetView(TimelineMath::Fit(start, end, 0.05, MIN_SPAN, static_cast<std::int32_t>(GetEndTick())));
    }

    float Timeline::GetX(double tick) const
    {
        return TimelineMath::Axis{view, lanesX0, lanesWidth}.ToX(tick);
    }

    void Timeline::Initialize()
    {
        Events::RegisterListener(EventType::OnDemoBoundsDetermined, [this]() {
            view = {0, static_cast<std::int32_t>(Components::Playback::GetTimelineEnd())};
            LOG_DEBUG("Set the timeline's view to {} to {}", view.start, view.end);
        });
        Events::RegisterListener(EventType::PostDemoLoad, [] {
            shownProperties.clear();
            TimelineGraph::Clear();
            drag = {};
        });
        RegisterCommands();
    }

    void Timeline::RegisterCommands()
    {
        using Commands::Bit;
        using Commands::Context;

        static constexpr std::array TIMESCALE_STEPS = Components::Playback::TIMESCALE_STEPS;
        const auto notInDemo = []() -> std::optional<std::string> {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo || !timescale)
                return "Load a demo first";
            return std::nullopt;
        };

        Commands::Register({
            .id = "playback.toggle",
            .label = "Play or pause",
            .category = "Playback",
            .defaults = {ImGuiKey_Space},
            .contexts = Bit(Context::Fly) | Bit(Context::Global),
            .unavailable = notInDemo,
            .run = [] { Components::Playback::TogglePaused(); },
            .keywords = "pause resume",
        });
        Commands::Register({
            .id = "playback.faster",
            .label = "Play faster",
            .category = "Playback",
            .defaults = {ImGuiKey_UpArrow},
            .unavailable = notInDemo,
            .run =
                [] {
                    float& value = timescale.value().value->floating_point;
                    if (const auto it = std::upper_bound(TIMESCALE_STEPS.begin(), TIMESCALE_STEPS.end(), value);
                        it != TIMESCALE_STEPS.end())
                        value = *it;
                },
            .keywords = "speed timescale",
        });
        Commands::Register({
            .id = "playback.slower",
            .label = "Play slower",
            .category = "Playback",
            .defaults = {ImGuiKey_DownArrow},
            .unavailable = notInDemo,
            .run =
                [] {
                    float& value = timescale.value().value->floating_point;
                    if (const auto it = std::upper_bound(TIMESCALE_STEPS.rbegin(), TIMESCALE_STEPS.rend(), value,
                                                         std::greater<float>());
                        it != TIMESCALE_STEPS.rend())
                        value = *it;
                },
            .keywords = "speed timescale slow motion",
        });

        // skips go on while the key is held, and stop at the capture range's ends and the frozen tick on the way
        const std::array<std::tuple<std::string_view, std::string_view, ImGuiKeyChord, std::int32_t>, 4> skips = {{
            {"playback.skipForward", "Skip forward 1 s", ImGuiKey_RightArrow, 1000},
            {"playback.skipBackward", "Skip back 1 s", ImGuiKey_LeftArrow, -1000},
            {"playback.skipForwardLong", "Skip forward 5 s", ImGuiMod_Shift | ImGuiKey_RightArrow, 5000},
            {"playback.skipBackwardLong", "Skip back 5 s", ImGuiMod_Shift | ImGuiKey_LeftArrow, -5000},
        }};
        for (const auto& [id, label, chord, ticks] : skips)
        {
            Commands::Register({
                .id = id,
                .label = label,
                .category = "Playback",
                .defaults = {chord},
                .repeat = true,
                .unavailable = notInDemo,
                .run =
                    [ticks] {
                        static bool lastSkipStopped = false;
                        const auto current = static_cast<std::int64_t>(Components::Playback::GetTimelineTick());
                        const auto target = current + ticks;
                        const auto [start, end] = GetCaptureRange();
                        // (the next press goes on past where this one stopped)
                        if (!lastSkipStopped)
                        {
                            for (const auto stop :
                                 {std::optional<std::int64_t>(start), std::optional<std::int64_t>(end),
                                  Components::Playback::GetFrozenTick()
                                      ? std::optional<std::int64_t>(*Components::Playback::GetFrozenTick())
                                      : std::nullopt})
                            {
                                if (stop && ((ticks > 0 && *stop > current && *stop < target) ||
                                             (ticks < 0 && *stop < current && *stop > target)))
                                {
                                    lastSkipStopped = true;
                                    Components::Playback::SetTickDelta(static_cast<std::int32_t>(*stop - current),
                                                                       true);
                                    return;
                                }
                            }
                        }
                        lastSkipStopped = false;
                        Components::Playback::SetTickDelta(ticks);
                    },
                .keywords = "seek jump rewind",
            });
        }

        const auto frameStep = [](int direction) {
            return [direction] {
                Seek(static_cast<std::int64_t>(Components::Playback::GetTimelineTick()) +
                         direction * static_cast<std::int64_t>(GetFrameTicks()),
                     false);
            };
        };
        Commands::Register({
            .id = "playback.frameBack",
            .label = "A frame back",
            .category = "Playback",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_LeftArrow},
            .repeat = true,
            .unavailable = notInDemo,
            .run = frameStep(-1),
            .keywords = "step previous frame",
        });
        Commands::Register({
            .id = "playback.frameForward",
            .label = "A frame forward",
            .category = "Playback",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_RightArrow},
            .repeat = true,
            .unavailable = notInDemo,
            .run = frameStep(1),
            .keywords = "step next frame",
        });
        Commands::Register({
            .id = "playback.freeze",
            .label = "Freeze time",
            .category = "Playback",
            .unavailable = notInDemo,
            .run =
                [] {
                    Components::Playback::ToggleFrozenTick();
                    if (const auto frozen = Components::Playback::GetFrozenTick())
                        Toasts::Show(std::format("Time frozen at {}", Toasts::FormatTime(*frozen)));
                },
            .keywords = "frozen tick lock stop",
        });
        Commands::Register({
            .id = "playback.bulletTime",
            .label = "Add bullet time here",
            .category = "Playback",
            .unavailable = notInDemo,
            .run = [] { AddBulletTime(Components::Playback::GetTimelineTick()); },
            .keywords = "freeze frozen span hold still matrix slow motion",
        });

        Commands::Register({
            .id = "kills.previous",
            .label = "Previous kill",
            .category = "Playback",
            .defaults = {ImGuiKey_Comma},
            .unavailable = notInDemo,
            .run = [] { Components::KillFinder::JumpToPrevious(); },
        });
        Commands::Register({
            .id = "kills.next",
            .label = "Next kill",
            .category = "Playback",
            .defaults = {ImGuiKey_Period},
            .unavailable = notInDemo,
            .run = [] { Components::KillFinder::JumpToNext(); },
        });
        Commands::Register({
            .id = "capture.setStart",
            .label = "Range starts here",
            .category = "Record",
            .defaults = {ImGuiKey_B},
            .unavailable = notInDemo,
            .run = [] { SetCaptureRangeEnd(true, Components::Playback::GetTimelineTick()); },
            .keywords = "record in point",
        });
        Commands::Register({
            .id = "capture.setEnd",
            .label = "Range ends here",
            .category = "Record",
            .defaults = {ImGuiKey_N},
            .unavailable = notInDemo,
            .run = [] { SetCaptureRangeEnd(false, Components::Playback::GetTimelineTick()); },
            .keywords = "record out point",
        });
        Commands::Register({
            .id = "capture.stop",
            .label = "Stop recording",
            .category = "Record",
            .defaults = {ImGuiKey_Escape},
            .contexts = Bit(Context::Capturing),
            .unavailable = []() -> std::optional<std::string> {
                if (!Components::CaptureManager::Get().IsCapturing())
                    return "Not recording";
                return std::nullopt;
            },
            .run = [] { Components::CaptureManager::Get().StopCapture(); },
        });

        Commands::Register({
            .id = "timeline.fit",
            .label = "Show the whole demo",
            .category = "Timeline",
            .defaults = {ImGuiKey_Backslash},
            .unavailable = notInDemo,
            .run = [this] { FitDemo(); },
            .keywords = "zoom out fit all",
        });
        Commands::Register({
            .id = "timeline.range",
            .label = "Show the capture range",
            .category = "Timeline",
            .defaults = {ImGuiMod_Shift | ImGuiKey_Backslash},
            .unavailable = notInDemo,
            .run = [this] { FitCaptureRange(); },
            .keywords = "zoom fit record",
        });
        Commands::Register({
            .id = "timeline.loop",
            .label = "Loop the capture range",
            .category = "Timeline",
            .unavailable = notInDemo,
            .run =
                [this] {
                    looping = !looping;
                    Toasts::Show(looping ? "Looping the capture range" : "Not looping");
                },
            .keywords = "repeat range",
        });
        Commands::Register({
            .id = "timeline.snap",
            .label = "Snap on the timeline",
            .category = "Timeline",
            .unavailable = notInDemo,
            .run = [this] { snapping = !snapping; },
            .keywords = "magnet",
        });
        Commands::Register({
            .id = "timeline.addMarker",
            .label = "Add a marker",
            .category = "Timeline",
            .defaults = {ImGuiKey_M},
            .contexts = Bit(Context::Fly) | Bit(Context::Global),
            .unavailable = notInDemo,
            .run = [] { AddMarkerAt(Components::Playback::GetTimelineTick()); },
            .keywords = "flag bookmark",
        });

        // the items of the shown tracks, by tick, for the keys before and after
        const auto itemTicks = [] {
            std::vector<std::int64_t> ticks;
            for (const auto& [property, keys] : KeyframeManager::Get().GetKeyframes())
            {
                if (IsShown(property, keys))
                    std::ranges::transform(keys, std::back_inserter(ticks),
                                           [](const Types::Keyframe& key) { return key.tick; });
            }
            for (const auto& actor : Components::Actors::GetActors())
            {
                std::ranges::transform(actor.clips, std::back_inserter(ticks),
                                       [](const auto& clip) { return clip.start; });
                std::ranges::transform(actor.faceClips, std::back_inserter(ticks),
                                       [](const auto& clip) { return clip.start; });
                std::ranges::transform(actor.events, std::back_inserter(ticks),
                                       [](const auto& event) { return event.tick; });
                std::ranges::transform(actor.keys, std::back_inserter(ticks), [](const auto& key) { return key.tick; });
            }
            std::ranges::transform(Components::Markers::GetAll(), std::back_inserter(ticks),
                                   [](const auto& marker) { return marker.tick; });
            std::ranges::sort(ticks);
            return ticks;
        };
        Commands::Register({
            .id = "timeline.previousKey",
            .label = "Go to the key before",
            .category = "Timeline",
            .defaults = {ImGuiKey_LeftBracket},
            .repeat = true,
            .unavailable = notInDemo,
            .run =
                [itemTicks] {
                    const auto ticks = itemTicks();
                    const auto current = static_cast<std::int64_t>(Components::Playback::GetTimelineTick());
                    const auto before = std::ranges::lower_bound(ticks, current);
                    if (before != ticks.begin())
                        JumpTo(static_cast<std::uint32_t>(*std::prev(before)));
                },
            .keywords = "previous node clip event marker",
        });
        Commands::Register({
            .id = "timeline.nextKey",
            .label = "Go to the key after",
            .category = "Timeline",
            .defaults = {ImGuiKey_RightBracket},
            .repeat = true,
            .unavailable = notInDemo,
            .run =
                [itemTicks] {
                    const auto ticks = itemTicks();
                    const auto current = static_cast<std::int64_t>(Components::Playback::GetTimelineTick());
                    const auto after = std::ranges::upper_bound(ticks, current);
                    if (after != ticks.end())
                        JumpTo(static_cast<std::uint32_t>(*after));
                },
            .keywords = "next node clip event marker",
        });

        const auto nudge = [](int direction) {
            return [direction] { TimelineSelection::MoveBy(direction * static_cast<std::int64_t>(GetFrameTicks())); };
        };
        const auto nothingSelected = []() -> std::optional<std::string> {
            if (TimelineSelection::Count() == 0)
                return "Select something on the timeline first";
            return std::nullopt;
        };
        Commands::Register({
            .id = "timeline.nudgeBack",
            .label = "Move the selection a frame back",
            .category = "Timeline",
            .defaults = {ImGuiMod_Alt | ImGuiKey_LeftArrow},
            .contexts = Bit(Context::Timeline),
            .repeat = true,
            .unavailable = nothingSelected,
            .run = nudge(-1),
            .keywords = "nudge earlier",
        });
        Commands::Register({
            .id = "timeline.nudgeForward",
            .label = "Move the selection a frame forward",
            .category = "Timeline",
            .defaults = {ImGuiMod_Alt | ImGuiKey_RightArrow},
            .contexts = Bit(Context::Timeline),
            .repeat = true,
            .unavailable = nothingSelected,
            .run = nudge(1),
            .keywords = "nudge later",
        });
        Commands::Register({
            .id = "timeline.start",
            .label = "Go to the start of the demo",
            .category = "Timeline",
            .defaults = {ImGuiKey_Home},
            .contexts = Bit(Context::Timeline),
            .unavailable = notInDemo,
            .run = [] { Seek(0, false); },
        });
        Commands::Register({
            .id = "timeline.end",
            .label = "Go to the end of the demo",
            .category = "Timeline",
            .defaults = {ImGuiKey_End},
            .contexts = Bit(Context::Timeline),
            .unavailable = notInDemo,
            .run = [] { Seek(GetEndTick(), false); },
        });
        Commands::Register({
            .id = "edit.selectAll",
            .label = "Select everything on the timeline",
            .category = "Edit",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_A},
            .contexts = Bit(Context::Timeline),
            .unavailable = notInDemo,
            .run =
                [] {
                    std::vector<Item> items;
                    for (const auto& [property, keys] : KeyframeManager::Get().GetKeyframes())
                    {
                        if (IsShown(property, keys))
                            for (const auto& key : keys)
                                items.push_back(TimelineSelection::KeyItem(property.type, key.id));
                    }
                    for (const auto& actor : Components::Actors::GetActors())
                        AppendRowItems(items, actor);
                    for (const auto& marker : Components::Markers::GetAll())
                        items.push_back({Kind::Marker, 0, marker.id});
                    TimelineSelection::Set(items);
                },
        });
        Commands::Register({
            .id = "edit.cut",
            .label = "Cut",
            .category = "Edit",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_X},
            .unavailable = nothingSelected,
            .run =
                [] {
                    TimelineSelection::Copy();
                    TimelineSelection::Delete();
                },
        });
        Commands::Register({
            .id = "edit.copy",
            .label = "Copy",
            .category = "Edit",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_C},
            .unavailable = nothingSelected,
            .run = [] { TimelineSelection::Copy(); },
        });

        // the keys and nodes to a file and back (File menu, the transport's menu)
        Commands::Register({
            .id = "file.importKeys",
            .label = "Import keys...",
            .category = "File",
            .unavailable = notInDemo,
            .run =
                [] {
                    const auto path = PathUtils::OpenFileDialog(false, OFN_EXPLORER | OFN_FILEMUSTEXIST,
                                                                "Keyframes (*.json)\0*.json\0", "json");
                    if (path)
                    {
                        Components::KeyframeSerializer::Read(*path);
                        LOG_INFO("Read keyframes from {}", path->string());
                        TimelineGraph::Refit();
                    }
                },
            .keywords = "open load keyframes campath file",
        });
        Commands::Register({
            .id = "file.exportKeys",
            .label = "Export keys...",
            .category = "File",
            .unavailable = [notInDemo]() -> std::optional<std::string> {
                if (auto reason = notInDemo())
                    return reason;
                const auto& all = KeyframeManager::Get().GetKeyframes();
                if (std::ranges::none_of(all, [](const auto& pair) { return !pair.second.empty(); }))
                    return "No keys or nodes to export yet";
                return std::nullopt;
            },
            .run =
                [] {
                    const auto path =
                        PathUtils::OpenFileDialog(true, OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT,
                                                  "Keyframes (*.json)\0*.json\0", "json");
                    if (path)
                    {
                        Components::KeyframeSerializer::Write(*path);
                        LOG_INFO("Wrote keyframes to {}", path->string());
                    }
                },
            .keywords = "save keyframes campath file",
        });
        Commands::Register({
            .id = "edit.paste",
            .label = "Paste at the playhead",
            .category = "Edit",
            .defaults = {ImGuiMod_Ctrl | ImGuiKey_V},
            .unavailable = [notInDemo]() -> std::optional<std::string> {
                if (auto reason = notInDemo())
                    return reason;
                if (TimelineSelection::CountCopied() == 0)
                    return "Copy something on the timeline first";
                return std::nullopt;
            },
            .run = [] { TimelineSelection::Paste(Components::Playback::GetTimelineTick()); },
        });
    }

    void Timeline::Render()
    {
        if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            return;
        if (!timescale)
        {
            timescale = Mod::GetGameInterface()->GetDvar("timescale");
            return;
        }

        // under the view and the panel, as tall as its splitter set it, which it is with the graph open and closed
        // (the graph needs room: Layout keeps a taller height for it)
        const auto rect = Layout::GetTimeline();
        SetPosition(rect.Min);
        SetSize(rect.GetSize());
        ImGui::SetNextWindowPos(GetPosition());
        ImGui::SetNextWindowSize(GetSize());

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse;
        // stays behind the tool windows (Preferences, Player Loadouts...) when clicked
        flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        const bool shown = ImGui::Begin("Timeline", nullptr, flags);
        ImGui::PopStyleVar();
        if (shown)
        {
            TimelineFrame frame{*this};
            frame.Run();
        }
        ImGui::End();

        // looping the capture range: back to its start once the playhead reaches its end
        const auto [start, end] = GetCaptureRange();
        const auto now = ImGui::GetTime();
        if (looping && !Components::Playback::IsPaused() && !Components::CaptureManager::Get().IsCapturing() &&
            !Components::Rewinding::IsRewinding() && start < end && now - loopedAt > 0.5 &&
            Components::Playback::GetTimelineTick() >= end)
        {
            loopedAt = now;
            Seek(start, false);
        }
    }

    void Timeline::Release()
    {
    }
}  // namespace IWXMVM::UI
