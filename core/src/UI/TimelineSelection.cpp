#include "StdInclude.hpp"
#include "TimelineSelection.hpp"

#include "Components/Actors.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Components/Playback.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/Lights.hpp"
#include "Components/Markers.hpp"
#include "Components/Props.hpp"
#include "Components/RuntimeProperties.hpp"
#include "Components/UndoHistory.hpp"
#include "UI/Toasts.hpp"

namespace IWXMVM::UI::TimelineSelection
{
    namespace
    {
        using Components::KeyframeManager;

        std::set<Item> selection;
        std::optional<Item> anchor;

        const Types::KeyframeableProperty& PropertyOf(std::uint32_t type)
        {
            return KeyframeManager::Get().GetProperty(static_cast<Types::KeyframeablePropertyType>(type));
        }

        Types::Keyframe* FindKey(const Item& item)
        {
            auto& keys = KeyframeManager::Get().GetKeyframes(PropertyOf(item.owner));
            const auto it = std::ranges::find(keys, static_cast<std::int32_t>(item.id), &Types::Keyframe::id);
            return it != keys.end() ? &*it : nullptr;
        }

        // the items of each actor, by its id
        std::map<std::uint32_t, std::vector<Item>> ByActor(std::span<const Item> items)
        {
            std::map<std::uint32_t, std::vector<Item>> byActor;
            for (const auto& item : items)
            {
                if (item.kind == Kind::Clip || item.kind == Kind::Event || item.kind == Kind::ActorKey ||
                    item.kind == Kind::FaceClip)
                    byActor[item.owner].push_back(item);
            }
            return byActor;
        }

        bool Has(std::span<const Item> items, Kind kind, std::int64_t id)
        {
            return std::ranges::any_of(items, [&](const Item& item) { return item.kind == kind && item.id == id; });
        }

        // what Copy took (or Duplicate copies), as it was
        struct Clipboard
        {
            struct Key
            {
                std::uint32_t property = 0;
                std::uint32_t tick = 0;
                Types::KeyframeValue value;
                Types::KeyInterpolation interpolation = Types::KeyInterpolation::Smooth;
                float easeIn = 0.0f;
                float easeOut = 0.0f;
            };
            std::vector<Key> keys;
            std::vector<std::pair<std::uint32_t, Types::ActorClip>> clips;
            std::vector<std::pair<std::uint32_t, Types::ActorClip>> faceClips;
            std::vector<std::pair<std::uint32_t, Types::ActorEvent>> events;
            std::vector<std::pair<std::uint32_t, Types::ActorKey>> actorKeys;
            std::vector<Components::Markers::Marker> markers;
            // a bar: the whole effect, light or prop, with a light's or prop's keys, as any may be gone by the time
            // they are pasted
            struct Bar
            {
                Types::Actor actor;
                Components::Lights::KeyCopy keys;
                Components::Props::KeyCopy propKeys;
            };
            std::vector<Bar> bars;

            std::size_t Count() const
            {
                return keys.size() + clips.size() + faceClips.size() + events.size() + actorKeys.size() +
                       markers.size() + bars.size();
            }

            // where it starts and ends, and whether it ends with a point in time (a key, an event, a marker) rather
            // than a clip's end
            std::tuple<std::int64_t, std::int64_t, bool> GetExtent() const
            {
                std::int64_t start = std::numeric_limits<std::int64_t>::max();
                std::int64_t end = std::numeric_limits<std::int64_t>::min();
                bool endsWithPoint = false;
                const auto extend = [&](std::int64_t from, std::int64_t to, bool point) {
                    start = std::min(start, from);
                    if (to > end || (to == end && point))
                        endsWithPoint = point;
                    end = std::max(end, to);
                };
                for (const auto& key : keys)
                    extend(key.tick, key.tick, true);
                // (actors' items are in the world's time, which bullet time puts later on the timeline)
                const auto at = [](std::uint32_t tick) { return Components::Playback::ActorToTimelineTick(tick); };
                for (const auto& [actor, clip] : clips)
                    extend(at(clip.start), at(clip.End()), false);
                for (const auto& [actor, clip] : faceClips)
                    extend(at(clip.start), at(clip.End()), false);
                for (const auto& [actor, event] : events)
                    extend(at(event.tick), at(event.End()), true);
                for (const auto& [actor, key] : actorKeys)
                    extend(at(key.tick), at(key.tick), true);
                for (const auto& marker : markers)
                    extend(marker.tick, marker.tick, true);
                for (const auto& bar : bars)
                {
                    if (const auto ticks = ActorLogic::GetBar(bar.actor))
                        extend(at(ticks->first), at(ticks->second), false);
                }
                return {start, end, endsWithPoint};
            }
        };
        Clipboard copied;

        Clipboard Take(std::span<const Item> items)
        {
            Clipboard taken;
            // (the keys of a light or prop whose bar goes too go with it)
            const auto withBar = [&](const Item& key) {
                const auto type = static_cast<Types::KeyframeablePropertyType>(key.owner);
                if (const auto light = Components::Lights::FindActor(type))
                    return std::ranges::find(items, Item{Kind::LightBar, *light, 0}) != items.end();
                if (const auto prop = Components::Props::FindActor(type))
                    return std::ranges::find(items, Item{Kind::PropBar, *prop, 0}) != items.end();
                return false;
            };
            for (const auto& item : items)
            {
                if (IsBar(item))
                {
                    if (const auto actor = Components::Actors::GetActor(item.owner))
                        taken.bars.push_back({*actor,
                                              actor->light ? Components::Lights::TakeKeys(actor->light->id)
                                                           : Components::Lights::KeyCopy{},
                                              actor->placedProp ? Components::Props::TakeKeys(actor->placedProp->id)
                                                                : Components::Props::KeyCopy{}});
                }
                else if (item.kind == Kind::Key)
                {
                    if (withBar(item))
                        continue;
                    if (const auto key = FindKey(item))
                        taken.keys.push_back(
                            {item.owner, key->tick, key->value, key->interpolation, key->easeIn, key->easeOut});
                }
                else if (item.kind == Kind::Marker)
                {
                    if (const auto marker = Components::Markers::Find(static_cast<std::uint32_t>(item.id)))
                        taken.markers.push_back(*marker);
                }
            }
            for (const auto& [actorId, actorItems] : ByActor(items))
            {
                const auto actor = Components::Actors::GetActor(actorId);
                if (!actor)
                    continue;
                for (const auto& clip : actor->clips)
                {
                    if (Has(actorItems, Kind::Clip, clip.id))
                        taken.clips.emplace_back(actorId, clip);
                }
                for (const auto& clip : actor->faceClips)
                {
                    if (Has(actorItems, Kind::FaceClip, clip.id))
                        taken.faceClips.emplace_back(actorId, clip);
                }
                for (const auto& event : actor->events)
                {
                    if (Has(actorItems, Kind::Event, event.id))
                        taken.events.emplace_back(actorId, event);
                }
                for (const auto& key : actor->keys)
                {
                    if (Has(actorItems, Kind::ActorKey, key.tick))
                        taken.actorKeys.emplace_back(actorId, key);
                }
            }
            return taken;
        }

        // a tick for a copied key that no key of its property has: the tick, or the first free one after it
        std::uint32_t FreeTick(const std::vector<Types::Keyframe>& keys, std::uint32_t tick)
        {
            while (std::ranges::find(keys, tick, &Types::Keyframe::tick) != keys.end())
                ++tick;
            return tick;
        }

        std::uint32_t Shifted(std::int64_t tick, std::int64_t offset)
        {
            return static_cast<std::uint32_t>(std::clamp<std::int64_t>(tick + offset, 0, 0xFFFFFFFFll));
        }

        // an actor's item moved by so many ticks of the timeline, which a bullet time span may lie in
        std::uint32_t ShiftedActor(std::uint32_t tick, std::int64_t offset)
        {
            return Components::Playback::ShiftActorTick(tick, offset);
        }

        // Adds the copies, offset ticks later, as one step; returns them. Copies of an actor that is gone are left
        // out.
        std::vector<Item> AddCopies(const Clipboard& clipboard, std::int64_t offset, const std::string& label)
        {
            std::vector<Item> added;
            auto& keyframeManager = KeyframeManager::Get();
            Components::UndoHistory::Get().BeginGroup(label);

            std::map<std::uint32_t, std::vector<Types::Keyframe>> keysByProperty;
            for (const auto& key : clipboard.keys)
            {
                const auto& property = PropertyOf(key.property);
                auto& copies = keysByProperty[key.property];
                // (apart from the keys there and the other copies)
                auto tick = FreeTick(keyframeManager.GetKeyframes(property), Shifted(key.tick, offset));
                while (std::ranges::find(copies, tick, &Types::Keyframe::tick) != copies.end())
                    tick = FreeTick(keyframeManager.GetKeyframes(property), tick + 1);
                auto& copy = copies.emplace_back(property, tick, key.value);
                copy.interpolation = key.interpolation;
                copy.easeIn = key.easeIn;
                copy.easeOut = key.easeOut;
            }
            for (auto& [type, copies] : keysByProperty)
            {
                const auto& property = PropertyOf(type);
                keyframeManager.AddKeyframes(property, copies);
                keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
                for (const auto& copy : copies)
                    added.push_back(KeyItem(property.type, copy.id));
            }

            std::map<std::uint32_t, Types::Actor> edited;
            const auto actorOf = [&](std::uint32_t id) -> Types::Actor* {
                if (const auto it = edited.find(id); it != edited.end())
                    return &it->second;
                const auto actor = Components::Actors::GetActor(id);
                return actor ? &edited.emplace(id, *actor).first->second : nullptr;
            };
            for (const auto& [actorId, clip] : clipboard.clips)
            {
                if (const auto actor = actorOf(actorId))
                {
                    auto copy = clip;
                    copy.id = 1;
                    for (const auto& other : actor->clips)
                        copy.id = std::max(copy.id, other.id + 1);
                    copy.start = ShiftedActor(clip.start, offset);
                    actor->clips.push_back(copy);
                    added.push_back({Kind::Clip, actorId, copy.id});
                }
            }
            for (const auto& [actorId, clip] : clipboard.faceClips)
            {
                if (const auto actor = actorOf(actorId))
                {
                    auto copy = clip;
                    copy.id = 1;
                    for (const auto& other : actor->faceClips)
                        copy.id = std::max(copy.id, other.id + 1);
                    copy.start = ShiftedActor(clip.start, offset);
                    actor->faceClips.push_back(copy);
                    added.push_back({Kind::FaceClip, actorId, copy.id});
                }
            }
            for (const auto& [actorId, event] : clipboard.events)
            {
                if (const auto actor = actorOf(actorId))
                {
                    auto copy = event;
                    copy.id = 1;
                    for (const auto& other : actor->events)
                        copy.id = std::max(copy.id, other.id + 1);
                    copy.tick = ShiftedActor(event.tick, offset);
                    actor->events.push_back(copy);
                    added.push_back({Kind::Event, actorId, copy.id});
                }
            }
            for (const auto& [actorId, key] : clipboard.actorKeys)
            {
                if (const auto actor = actorOf(actorId))
                {
                    auto copy = key;
                    copy.tick = ShiftedActor(key.tick, offset);
                    while (std::ranges::find(actor->keys, copy.tick, &Types::ActorKey::tick) != actor->keys.end())
                        ++copy.tick;
                    actor->keys.push_back(copy);
                    added.push_back({Kind::ActorKey, actorId, copy.tick});
                }
            }
            for (const auto& [id, actor] : edited)
                Components::Actors::Update(actor, false, label);

            for (const auto& bar : clipboard.bars)
            {
                auto copy = bar.actor;
                copy.name = Components::Actors::MakeName(copy.name);
                if (const auto ticks = ActorLogic::GetBar(copy))
                    ActorLogic::MoveBar(copy, ShiftedActor(ticks->first, offset));
                // (a light or prop of its own, with its own keys)
                if (copy.light)
                    copy.light->id = 0;
                if (copy.placedProp)
                    copy.placedProp->id = 0;
                const auto id = Components::Actors::Add(copy);
                const auto actor = Components::Actors::GetActor(id);
                if (actor && actor->light)
                    Components::Lights::PutKeys(actor->light->id, bar.keys, offset);
                if (actor && actor->placedProp)
                    Components::Props::PutKeys(actor->placedProp->id, bar.propKeys, offset);
                if (actor)
                    added.push_back(BarItem(*actor));
            }

            for (const auto& marker : clipboard.markers)
                added.push_back({Kind::Marker, 0, Components::Markers::Add(Shifted(marker.tick, offset), marker.name)});

            Components::UndoHistory::Get().EndGroup();
            Components::Actors::EndEditStep();
            return added;
        }

        // A drag of the selection, from where it was when it started
        struct MoveState
        {
            struct Key
            {
                std::uint32_t property = 0;
                std::int32_t id = 0;
                std::uint32_t tick = 0;
            };
            std::vector<Key> keys;
            // the actors as they were, and their items being moved
            std::map<std::uint32_t, Types::Actor> actors;
            std::map<std::uint32_t, std::vector<Item>> actorItems;
            // their keys' ticks now, by the ticks they had (the selection names them by tick)
            std::map<std::pair<std::uint32_t, std::int64_t>, std::int64_t> actorKeyTicks;
            std::vector<Components::Markers::Marker> markersBefore;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> markers;
            std::vector<TimelineMath::Span> spans;
            std::string label;
            std::int64_t delta = 0;
        };
        std::optional<MoveState> moving;

        void ApplyMove(std::int64_t delta)
        {
            auto& state = *moving;
            state.delta = delta;
            const int direction = delta < 0 ? -1 : 1;
            auto& keyframeManager = KeyframeManager::Get();

            // keys, a property at a time, kept apart from the others
            std::set<std::uint32_t> properties;
            for (const auto& key : state.keys)
                properties.insert(key.property);
            for (const auto type : properties)
            {
                auto& keys = keyframeManager.GetKeyframes(PropertyOf(type));
                std::vector<std::int64_t> ticks;
                std::vector<bool> moved;
                for (const auto& keyframe : keys)
                {
                    const auto it = std::ranges::find_if(state.keys, [&](const MoveState::Key& key) {
                        return key.property == type && key.id == keyframe.id;
                    });
                    moved.push_back(it != state.keys.end());
                    ticks.push_back(it != state.keys.end() ? it->tick + delta : keyframe.tick);
                }
                TimelineMath::SeparateTicks(ticks, moved, direction);
                for (std::size_t i = 0; i < keys.size(); ++i)
                    keys[i].tick = static_cast<std::uint32_t>(ticks[i]);
                keyframeManager.SortAndSaveKeyframes(keys);
            }

            for (const auto& [actorId, original] : state.actors)
            {
                auto edited = original;
                const auto& items = state.actorItems[actorId];
                // clips first: an event that is a Hit moves its death with it, unless that moved already
                for (auto& clip : edited.clips)
                {
                    if (Has(items, Kind::Clip, clip.id))
                        clip.start = ShiftedActor(clip.start, delta);
                }
                for (auto& clip : edited.faceClips)
                {
                    if (Has(items, Kind::FaceClip, clip.id))
                        clip.start = ShiftedActor(clip.start, delta);
                }
                for (const auto& event : original.events)
                {
                    if (Has(items, Kind::Event, event.id))
                        ActorLogic::MoveEvent(edited, event.id, ShiftedActor(event.tick, delta));
                }
                std::vector<std::int64_t> ticks;
                std::vector<bool> moved;
                for (const auto& key : original.keys)
                {
                    const bool selected = Has(items, Kind::ActorKey, key.tick);
                    moved.push_back(selected);
                    ticks.push_back(selected ? ShiftedActor(key.tick, delta) : key.tick);
                }
                TimelineMath::SeparateTicks(ticks, moved, direction);
                for (std::size_t i = 0; i < original.keys.size(); ++i)
                {
                    edited.keys[i].tick = static_cast<std::uint32_t>(ticks[i]);
                    if (!moved[i])
                        continue;
                    // the selection follows the key
                    auto& now = state.actorKeyTicks[{actorId, original.keys[i].tick}];
                    selection.erase({Kind::ActorKey, actorId, now});
                    now = ticks[i];
                    selection.insert({Kind::ActorKey, actorId, now});
                }
                // (a light's keys go with its bar: they are among the keys above)
                if (const auto bar = ActorLogic::GetBar(original);
                    bar &&
                    (Has(items, Kind::EffectBar, 0) || Has(items, Kind::LightBar, 0) || Has(items, Kind::PropBar, 0)))
                    ActorLogic::MoveBar(edited, ShiftedActor(bar->first, delta));
                Components::Actors::Update(edited, false, state.label);
            }

            for (const auto& [id, tick] : state.markers)
                Components::Markers::SetTick(id, Shifted(tick, delta));
        }
    }  // namespace

    Item KeyItem(Types::KeyframeablePropertyType property, std::int32_t keyframeId)
    {
        return {Kind::Key, static_cast<std::uint32_t>(property), keyframeId};
    }

    Item BarItem(const Types::Actor& actor)
    {
        return {actor.light ? Kind::LightBar : actor.placedProp ? Kind::PropBar : Kind::EffectBar, actor.id, 0};
    }

    bool IsBar(const Item& item)
    {
        return item.kind == Kind::EffectBar || item.kind == Kind::LightBar || item.kind == Kind::PropBar;
    }

    PanelLogic::Kind GetPanelKind(const Item& item)
    {
        switch (item.kind)
        {
            case Kind::Key:
                if (item.owner == static_cast<std::uint32_t>(Types::KeyframeablePropertyType::CampathCamera))
                    return PanelLogic::Kind::Node;
                if (Components::RuntimeProperties::IsActorProperty(
                        static_cast<Types::KeyframeablePropertyType>(item.owner)))
                    return PanelLogic::Kind::LightKey;
                return Components::BoneCameraLogic::IsKeyedProperty(
                           static_cast<Types::KeyframeablePropertyType>(item.owner))
                           ? PanelLogic::Kind::CameraKey
                           : PanelLogic::Kind::Key;
            case Kind::Clip:
            case Kind::FaceClip:
                return PanelLogic::Kind::Clip;
            case Kind::Event:
                return PanelLogic::Kind::Event;
            case Kind::ActorKey:
                return PanelLogic::Kind::ActorKey;
            case Kind::EffectBar:
            case Kind::LightBar:
            case Kind::PropBar:
                return PanelLogic::Kind::Actor;
            default:
                return PanelLogic::Kind::Marker;
        }
    }

    const std::set<Item>& Get()
    {
        return selection;
    }

    bool IsSelected(const Item& item)
    {
        return selection.contains(item);
    }

    void Set(std::span<const Item> items)
    {
        selection = {items.begin(), items.end()};
    }

    void Add(std::span<const Item> items)
    {
        selection.insert(items.begin(), items.end());
    }

    void Toggle(const Item& item)
    {
        if (!selection.erase(item))
            selection.insert(item);
    }

    void Clear()
    {
        selection.clear();
        anchor.reset();
        // (another demo loading mid-drag: what it moved is gone)
        if (moving)
        {
            moving.reset();
            Components::UndoHistory::Get().EndGroup();
        }
    }

    std::optional<Item> GetAnchor()
    {
        return anchor;
    }

    void SetAnchor(std::optional<Item> item)
    {
        anchor = item;
    }

    std::optional<TimelineMath::Span> GetSpan(const Item& item)
    {
        switch (item.kind)
        {
            case Kind::Key:
                if (const auto key = FindKey(item))
                    return TimelineMath::Span{key->tick, key->tick};
                return std::nullopt;
            case Kind::Marker:
                if (const auto marker = Components::Markers::Find(static_cast<std::uint32_t>(item.id)))
                    return TimelineMath::Span{marker->tick, marker->tick};
                return std::nullopt;
            default:
                break;
        }
        const auto actor = Components::Actors::GetActor(item.owner);
        if (!actor)
            return std::nullopt;
        if (IsBar(item))
        {
            if (const auto bar = ActorLogic::GetBar(*actor))
                return TimelineMath::Span{Components::Playback::ActorToTimelineTick(bar->first),
                                          Components::Playback::ActorToTimelineTick(bar->second)};
            return std::nullopt;
        }
        if (item.kind == Kind::Clip || item.kind == Kind::FaceClip)
        {
            const auto& clips = item.kind == Kind::Clip ? actor->clips : actor->faceClips;
            const auto clip = std::ranges::find(clips, static_cast<std::uint32_t>(item.id), &Types::ActorClip::id);
            if (clip != clips.end())
                return TimelineMath::Span{Components::Playback::ActorToTimelineTick(clip->start),
                                          Components::Playback::ActorToTimelineTick(clip->End())};
        }
        else if (item.kind == Kind::Event)
        {
            const auto event =
                std::ranges::find(actor->events, static_cast<std::uint32_t>(item.id), &Types::ActorEvent::id);
            if (event != actor->events.end())
                return TimelineMath::Span{Components::Playback::ActorToTimelineTick(event->tick),
                                          Components::Playback::ActorToTimelineTick(event->End())};
        }
        else if (std::ranges::find(actor->keys, static_cast<std::uint32_t>(item.id), &Types::ActorKey::tick) !=
                 actor->keys.end())
        {
            const auto tick = Components::Playback::ActorToTimelineTick(static_cast<std::uint32_t>(item.id));
            return TimelineMath::Span{tick, tick};
        }
        return std::nullopt;
    }

    std::size_t Count()
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(selection, [](const Item& item) { return GetSpan(item).has_value(); }));
    }

    void Prune()
    {
        std::erase_if(selection, [](const Item& item) { return !GetSpan(item).has_value(); });
        if (anchor && !GetSpan(*anchor))
            anchor.reset();
    }

    std::optional<std::int32_t> GetOnlyNode()
    {
        if (selection.size() != 1)
            return std::nullopt;
        const auto& item = *selection.begin();
        if (item.kind != Kind::Key ||
            item.owner != static_cast<std::uint32_t>(Types::KeyframeablePropertyType::CampathCamera) || !GetSpan(item))
            return std::nullopt;
        return static_cast<std::int32_t>(item.id);
    }

    std::string Describe(std::span<const Item> items)
    {
        if (items.empty())
            return "nothing";
        const auto nounOf = [](const Item& item) -> std::string_view {
            switch (item.kind)
            {
                case Kind::Key:
                    return item.owner == static_cast<std::uint32_t>(Types::KeyframeablePropertyType::CampathCamera)
                               ? "node"
                               : "key";
                case Kind::ActorKey:
                    return "key";
                case Kind::Clip:
                    return "clip";
                case Kind::FaceClip:
                    return "face clip";
                case Kind::Event:
                    return "event";
                case Kind::Marker:
                    return "marker";
                case Kind::EffectBar:
                    return "effect";
                case Kind::LightBar:
                    return "light";
                case Kind::PropBar:
                    return "prop";
            }
            return "item";
        };
        auto noun = nounOf(items.front());
        for (const auto& item : items)
        {
            if (nounOf(item) != noun)
                noun = "item";
        }
        if (items.size() == 1)
            return std::string(noun);
        return std::format("{} {}s", items.size(), noun);
    }

    std::size_t Delete()
    {
        Prune();
        if (selection.empty())
            return 0;
        const std::vector<Item> items(selection.begin(), selection.end());
        const auto label = "delete " + Describe(items);
        auto& keyframeManager = KeyframeManager::Get();

        Components::UndoHistory::Get().BeginGroup(label);
        std::map<std::uint32_t, std::vector<Types::Keyframe>> keysByProperty;
        for (const auto& item : items)
        {
            if (item.kind == Kind::Key)
            {
                if (const auto key = FindKey(item))
                    keysByProperty[item.owner].push_back(*key);
            }
        }
        for (const auto& [type, keys] : keysByProperty)
        {
            const auto& property = PropertyOf(type);
            keyframeManager.RemoveKeyframes(property, keys);
            keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
        }

        const auto selectedClip = Components::Actors::GetSelectedClip();
        const auto selectedFaceClip = Components::Actors::GetSelectedFaceClip();
        const auto selectedEvent = Components::Actors::GetSelectedEvent();
        for (const auto& [actorId, actorItems] : ByActor(items))
        {
            auto actor = Components::Actors::GetActor(actorId);
            if (!actor)
                continue;
            std::erase_if(actor->clips,
                          [&](const Types::ActorClip& clip) { return Has(actorItems, Kind::Clip, clip.id); });
            std::erase_if(actor->faceClips,
                          [&](const Types::ActorClip& clip) { return Has(actorItems, Kind::FaceClip, clip.id); });
            std::erase_if(actor->events,
                          [&](const Types::ActorEvent& event) { return Has(actorItems, Kind::Event, event.id); });
            std::erase_if(actor->keys,
                          [&](const Types::ActorKey& key) { return Has(actorItems, Kind::ActorKey, key.tick); });
            Components::Actors::Update(*actor, false, label);
            if (Components::Actors::GetSelected() == actorId)
            {
                if (selectedClip && Has(actorItems, Kind::Clip, *selectedClip))
                    Components::Actors::SelectClip(std::nullopt);
                if (selectedFaceClip && Has(actorItems, Kind::FaceClip, *selectedFaceClip))
                    Components::Actors::SelectFaceClip(std::nullopt);
                if (selectedEvent && Has(actorItems, Kind::Event, *selectedEvent))
                    Components::Actors::SelectEvent(std::nullopt);
            }
        }

        std::vector<std::uint32_t> markers;
        for (const auto& item : items)
        {
            if (item.kind == Kind::Marker)
                markers.push_back(static_cast<std::uint32_t>(item.id));
            // (a light's keys go with it)
            if (IsBar(item))
                Components::Actors::Remove(item.owner);
        }
        if (!markers.empty())
            Components::Markers::Remove(markers, label);
        Components::UndoHistory::Get().EndGroup();
        Components::Actors::EndEditStep();

        selection.clear();
        anchor.reset();
        Toasts::Show("Deleted " + Describe(items), true);
        return items.size();
    }

    std::size_t Duplicate()
    {
        Prune();
        if (selection.empty())
            return 0;
        const std::vector<Item> items(selection.begin(), selection.end());
        const auto clipboard = Take(items);
        // right after the selection: after its last clip's end, or half a second after its last key
        const auto [start, end, endsWithPoint] = clipboard.GetExtent();
        const auto offset = end - start + (endsWithPoint ? 500 : 0);
        const auto added = AddCopies(clipboard, offset, "duplicate " + Describe(items));
        Set(added);
        anchor.reset();
        Toasts::Show("Duplicated " + Describe(items), true);
        return added.size();
    }

    std::size_t Copy()
    {
        Prune();
        if (selection.empty())
            return 0;
        const std::vector<Item> items(selection.begin(), selection.end());
        copied = Take(items);
        Toasts::Show("Copied " + Describe(items));
        return copied.Count();
    }

    std::size_t Paste(std::uint32_t tick)
    {
        if (copied.Count() == 0)
            return 0;
        // named as the items it pastes would be
        std::vector<Item> pasting;
        for (const auto& key : copied.keys)
            pasting.push_back({Kind::Key, key.property});
        for (const auto& [actor, clip] : copied.clips)
            pasting.push_back({Kind::Clip, actor});
        for (const auto& [actor, clip] : copied.faceClips)
            pasting.push_back({Kind::FaceClip, actor});
        for (const auto& [actor, event] : copied.events)
            pasting.push_back({Kind::Event, actor});
        for (const auto& [actor, key] : copied.actorKeys)
            pasting.push_back({Kind::ActorKey, actor});
        for (const auto& marker : copied.markers)
            pasting.push_back({Kind::Marker});
        for (const auto& bar : copied.bars)
            pasting.push_back(BarItem(bar.actor));
        const auto start = std::get<0>(copied.GetExtent());
        const auto added = AddCopies(copied, static_cast<std::int64_t>(tick) - start, "paste " + Describe(pasting));
        if (added.empty())
        {
            Toasts::Show("Nothing to paste: the actors it was copied from are gone");
            return 0;
        }
        Set(added);
        anchor.reset();
        Toasts::Show(std::format("Pasted {} at {}", Describe(added), Toasts::FormatTime(tick)), true);
        return added.size();
    }

    std::size_t CountCopied()
    {
        return copied.Count();
    }

    void ForgetCopied()
    {
        copied = {};
    }

    void BeginMove()
    {
        Prune();
        if (moving || selection.empty())
            return;
        const std::vector<Item> items(selection.begin(), selection.end());
        MoveState state;
        state.label = "move " + Describe(items);
        for (const auto& item : items)
        {
            if (const auto span = GetSpan(item))
                state.spans.push_back(*span);
            if (item.kind == Kind::Key)
            {
                if (const auto key = FindKey(item))
                    state.keys.push_back({item.owner, key->id, key->tick});
            }
            else if (item.kind == Kind::Marker)
            {
                if (const auto marker = Components::Markers::Find(static_cast<std::uint32_t>(item.id)))
                    state.markers.emplace_back(marker->id, marker->tick);
            }
        }
        for (const auto& [actorId, actorItems] : ByActor(items))
        {
            if (const auto actor = Components::Actors::GetActor(actorId))
            {
                state.actors.emplace(actorId, *actor);
                state.actorItems[actorId] = actorItems;
                for (const auto& item : actorItems)
                {
                    if (item.kind == Kind::ActorKey)
                        state.actorKeyTicks[{actorId, item.id}] = item.id;
                }
            }
        }
        for (const auto& item : items)
        {
            const auto actor = IsBar(item) ? Components::Actors::GetActor(item.owner) : std::nullopt;
            if (!actor)
                continue;
            state.actors.emplace(actor->id, *actor);
            state.actorItems[actor->id].push_back(item);
            if (!actor->light && !actor->placedProp)
                continue;
            // its keys go with it, or a fade would stay behind, each once (they may be selected too)
            for (const auto type : actor->light ? Components::Lights::GetKeyedProperties(actor->light->id)
                                                : Components::Props::GetKeyedProperties(actor->placedProp->id))
            {
                const auto owner = static_cast<std::uint32_t>(type);
                for (const auto& key : KeyframeManager::Get().GetKeyframes(PropertyOf(owner)))
                {
                    const auto same = [&](const MoveState::Key& each) {
                        return each.property == owner && each.id == key.id;
                    };
                    if (std::ranges::any_of(state.keys, same))
                        continue;
                    state.keys.push_back({owner, key.id, key.tick});
                    state.spans.push_back({key.tick, key.tick});
                }
            }
        }
        state.markersBefore = Components::Markers::GetAll();

        Components::UndoHistory::Get().BeginGroup(state.label);
        auto& keyframeManager = KeyframeManager::Get();
        moving = std::move(state);
        for (const auto& key : moving->keys)
        {
            auto& keys = keyframeManager.GetKeyframes(PropertyOf(key.property));
            if (const auto it = std::ranges::find(keys, key.id, &Types::Keyframe::id); it != keys.end())
                keyframeManager.BeginModifyingKeyframeTick(*it);
        }
    }

    void UpdateMove(std::int64_t delta)
    {
        if (moving && delta != moving->delta)
            ApplyMove(delta);
    }

    void EndMove()
    {
        if (!moving)
            return;
        auto& keyframeManager = KeyframeManager::Get();
        for (const auto& key : moving->keys)
        {
            const auto& property = PropertyOf(key.property);
            auto& keys = keyframeManager.GetKeyframes(property);
            if (const auto it = std::ranges::find(keys, key.id, &Types::Keyframe::id); it != keys.end())
                keyframeManager.EndModifyingKeyframeTick(property, *it);
        }
        if (!moving->markers.empty())
            Components::Markers::PushMove(moving->markersBefore, moving->label);
        Components::UndoHistory::Get().EndGroup();
        Components::Actors::EndEditStep();
        moving.reset();
    }

    bool IsMoving()
    {
        return moving.has_value();
    }

    std::vector<TimelineMath::Span> GetMovingSpans()
    {
        return moving ? moving->spans : std::vector<TimelineMath::Span>{};
    }

    void MoveBy(std::int64_t delta)
    {
        BeginMove();
        if (!moving)
            return;
        // (no further back than the start)
        std::int64_t earliest = std::numeric_limits<std::int64_t>::max();
        for (const auto& span : moving->spans)
            earliest = std::min(earliest, span.start);
        UpdateMove(std::max(delta, -earliest));
        EndMove();
    }

    namespace
    {
        // the keys selected whose interpolation and eases can change (a switch's hold), of each property, as they
        // are now
        std::map<std::uint32_t, std::vector<Types::Keyframe>> GetSelectedKeys()
        {
            std::map<std::uint32_t, std::vector<Types::Keyframe>> keys;
            for (const auto& item : selection)
            {
                if (item.kind != Kind::Key ||
                    KeyframeLogic::IsStepped(static_cast<Types::KeyframeablePropertyType>(item.owner)))
                    continue;
                if (const auto key = FindKey(item))
                    keys[item.owner].push_back(*key);
            }
            return keys;
        }

        // Changes each key selected as one step, named "<what> <the keys><after>" ("make 3 nodes Linear"), and says
        // so with "<the keys> <done>" (unless told not to); returns how many it changed
        std::size_t ChangeKeys(const std::function<void(Types::Keyframe&)>& change, std::string_view what,
                               std::string_view after, std::string_view done, bool message = true)
        {
            const auto byProperty = GetSelectedKeys();
            std::vector<Item> items;
            for (const auto& [type, keys] : byProperty)
            {
                for (const auto& key : keys)
                    items.push_back(KeyItem(static_cast<Types::KeyframeablePropertyType>(type), key.id));
            }
            if (items.empty())
                return 0;
            const auto described = Describe(items);
            const auto label = std::format("{} {}{}{}", what, items.size() == 1 ? "the " : "", described, after);
            auto& keyframeManager = KeyframeManager::Get();
            Components::UndoHistory::Get().BeginGroup(label);
            for (auto [type, keys] : byProperty)
            {
                for (auto& key : keys)
                    change(key);
                keyframeManager.ChangeKeyframes(PropertyOf(type), keys, label);
            }
            Components::UndoHistory::Get().EndGroup();
            if (message)
            {
                auto text = std::format("{} {}", described, done);
                text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
                Toasts::Show(text, true);
            }
            return items.size();
        }

        // the same amount for all, none when they differ
        void Share(std::optional<float>& shared, float amount, bool first)
        {
            shared = first || shared == amount ? std::optional(amount) : std::nullopt;
        }
    }  // namespace

    KeyOptions GetKeyOptions()
    {
        KeyOptions options;
        for (const auto& [type, keys] : GetSelectedKeys())
        {
            for (const auto& key : keys)
            {
                options.interpolation = options.keys == 0 || options.interpolation == key.interpolation
                                            ? std::optional(key.interpolation)
                                            : std::nullopt;
                options.anyAuto |= key.interpolation == Types::KeyInterpolation::Auto;
                options.easedIn = (options.keys == 0 || options.easedIn) && key.easeIn > 0.0f;
                options.easedOut = (options.keys == 0 || options.easedOut) && key.easeOut > 0.0f;
                Share(options.easeIn, key.easeIn, options.keys == 0);
                Share(options.easeOut, key.easeOut, options.keys == 0);
                ++options.keys;
            }
        }
        return options;
    }

    std::size_t SetInterpolation(Types::KeyInterpolation interpolation)
    {
        const auto name = magic_enum::enum_name(interpolation);
        return ChangeKeys([interpolation](Types::Keyframe& key) { key.interpolation = interpolation; }, "make",
                          std::format(" {}", name), std::format("now {}", name));
    }

    std::size_t SetEase(bool in, float amount, bool message)
    {
        const auto end = in ? "in" : "out";
        amount = std::clamp(amount, 0.0f, 1.0f);
        // (some of the way: "now ease in 40%")
        const auto part = amount > 0.0f && amount < 1.0f ? std::format(" {:.0f}%", amount * 100.0f) : std::string();
        return ChangeKeys(
            [in, amount](Types::Keyframe& key) { (in ? key.easeIn : key.easeOut) = amount; },
            amount > 0.0f ? std::format("ease {}", end) : std::format("no ease {} on", end), part,
            amount > 0.0f ? std::format("now ease {}{}", end, part) : std::format("no longer ease {}", end), message);
    }
}  // namespace IWXMVM::UI::TimelineSelection
