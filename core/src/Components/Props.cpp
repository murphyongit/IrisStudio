#include "StdInclude.hpp"
#include "Props.hpp"

#include "Actors.hpp"
#include "Events.hpp"
#include "KeyframeManager.hpp"
#include "Playback.hpp"

namespace IWXMVM::Components::Props
{
    namespace
    {
        std::atomic<std::shared_ptr<const AllKeys>> keysNow;
        std::atomic<std::shared_ptr<const std::set<std::uint32_t>>> keyedNow;
        std::mutex movesMutex;
        std::vector<KeyedMove> keyedMoves;

        std::mutex previewMutex;
        std::optional<Preview> preview;
        std::uint64_t previewFrame = 0;
        std::atomic<std::uint64_t> frame = 0;

        std::optional<Types::KeyframeablePropertyType> FindProperty(std::uint32_t prop, PropLogic::Channel channel)
        {
            return RuntimeProperties::FindById(PropLogic::GetPropertyId(prop, channel));
        }
    }  // namespace

    void Initialize()
    {
        keysNow = std::make_shared<const AllKeys>();
        keyedNow = std::make_shared<const std::set<std::uint32_t>>();

        Events::RegisterListener(EventType::OnFrame, [] {
            ++frame;
            auto& keyframeManager = KeyframeManager::Get();
            auto all = std::make_shared<AllKeys>();
            for (const auto& actor : Actors::GetActors())
            {
                if (!actor.placedProp)
                    continue;
                PropLogic::Keys keys;
                auto any = false;
                for (std::size_t i = 0; i < PropLogic::ALL_CHANNELS.size(); ++i)
                {
                    const auto type = FindProperty(actor.placedProp->id, PropLogic::ALL_CHANNELS[i]);
                    if (!type || !keyframeManager.HasProperty(*type))
                        continue;
                    keys[i] = keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type));
                    any = any || !keys[i].empty();
                }
                if (any)
                    all->emplace(actor.placedProp->id, std::move(keys));
            }
            keysNow = std::move(all);

            auto keyed = std::make_shared<std::set<std::uint32_t>>();
            for (const auto type : RuntimeProperties::GetAll())
            {
                if (!RuntimeProperties::IsProp(type) || !keyframeManager.HasProperty(type) ||
                    keyframeManager.GetKeyframes(keyframeManager.GetProperty(type)).empty())
                    continue;
                if (const auto prop = PropLogic::ParseProp(RuntimeProperties::Find(type)->id))
                    keyed->insert(*prop);
            }
            keyedNow = std::move(keyed);
        });

        // a key added on a timeline row without keys takes the prop's value
        KeyframeManager::Get().AddUnkeyedValues(
            [](Types::KeyframeablePropertyType type) -> std::optional<Types::KeyframeValue> {
                const auto description = RuntimeProperties::Find(type);
                if (!description || !RuntimeProperties::IsProp(type))
                    return std::nullopt;
                for (const auto& actor : Actors::GetActors())
                {
                    if (!actor.placedProp)
                        continue;
                    for (const auto channel : PropLogic::ALL_CHANNELS)
                        if (PropLogic::GetPropertyId(actor.placedProp->id, channel) == description->id)
                            return PropLogic::GetValue(actor, channel);
                }
                return std::nullopt;
            });
    }

    const Types::KeyframeableProperty& GetProperty(std::uint32_t prop, PropLogic::Channel channel)
    {
        return KeyframeManager::Get().AddProperty(RuntimeProperties::Register(PropLogic::Describe(prop, channel)));
    }

    std::shared_ptr<const AllKeys> GetKeys()
    {
        return keysNow.load();
    }

    std::shared_ptr<const std::set<std::uint32_t>> GetKeyed()
    {
        return keyedNow.load();
    }

    void CopyKeys(std::uint32_t from, std::uint32_t to)
    {
        PutKeys(to, TakeKeys(from), 0);
    }

    KeyCopy TakeKeys(std::uint32_t prop)
    {
        auto& keyframeManager = KeyframeManager::Get();
        KeyCopy copy;
        for (const auto channel : PropLogic::ALL_CHANNELS)
        {
            const auto type = FindProperty(prop, channel);
            if (!type || !keyframeManager.HasProperty(*type))
                continue;
            const auto& keys = keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type));
            if (!keys.empty())
                copy.emplace_back(channel, keys);
        }
        return copy;
    }

    void PutKeys(std::uint32_t prop, const KeyCopy& keys, std::int64_t ticks)
    {
        auto& keyframeManager = KeyframeManager::Get();
        for (const auto& [channel, channelKeys] : keys)
        {
            const auto& property = GetProperty(prop, channel);
            for (const auto& key : channelKeys)
            {
                auto copy = Types::Keyframe(
                    property, static_cast<std::uint32_t>(std::max<std::int64_t>(key.tick + ticks, 0)), key.value);
                copy.interpolation = key.interpolation;
                copy.easeIn = key.easeIn;
                copy.easeOut = key.easeOut;
                keyframeManager.AddKeyframe(property, copy);
            }
            keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
        }
    }

    std::vector<Types::KeyframeablePropertyType> GetKeyedProperties(std::uint32_t prop)
    {
        auto& keyframeManager = KeyframeManager::Get();
        std::vector<Types::KeyframeablePropertyType> properties;
        for (const auto channel : PropLogic::ALL_CHANNELS)
        {
            const auto type = FindProperty(prop, channel);
            if (type && keyframeManager.HasProperty(*type) &&
                !keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type)).empty())
                properties.push_back(*type);
        }
        return properties;
    }

    Types::Actor WithKeyedPlacement(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto keys = GetKeys();
        if (!actor.placedProp || !keys)
            return actor;
        const auto found = keys->find(actor.placedProp->id);
        return found == keys->end() ? actor : PropLogic::ApplyKeys(actor, found->second, tick);
    }

    Types::Actor WithKeyedPlacementNow(const Types::Actor& actor, std::uint32_t tick)
    {
        if (!actor.placedProp)
            return actor;
        auto& keyframeManager = KeyframeManager::Get();
        PropLogic::Keys keys;
        for (std::size_t i = 0; i < PropLogic::ALL_CHANNELS.size(); ++i)
        {
            const auto type = FindProperty(actor.placedProp->id, PropLogic::ALL_CHANNELS[i]);
            if (type && keyframeManager.HasProperty(*type))
                keys[i] = keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type));
        }
        return PropLogic::ApplyKeys(actor, keys, tick);
    }

    bool MoveTo(Types::Actor& actor, const Types::Actor& placed, const std::string& step)
    {
        if (!actor.placedProp || !placed.placedProp)
            return false;
        const auto before = actor;
        const auto all = GetKeys();
        const auto found = all ? all->find(actor.placedProp->id) : AllKeys::const_iterator{};
        const auto keyed = [&](PropLogic::Channel channel) {
            return all && found != all->end() && !found->second[static_cast<std::size_t>(channel)].empty();
        };
        // (what it moved from: where its keys have it at the playhead)
        const auto from = WithKeyedPlacement(actor, Playback::GetTimelineTick());
        KeyedMove move{.actor = actor.id, .step = step};
        for (const auto channel : PropLogic::ALL_CHANNELS)
        {
            const auto value = PropLogic::GetValue(placed, channel);
            if (value.vector3 == PropLogic::GetValue(from, channel).vector3)
                continue;
            if (keyed(channel))
                (channel == PropLogic::Channel::Position ? move.position : move.turn) = value.vector3;
            else
                PropLogic::SetValue(actor, channel, value);
        }
        if (move.position || move.turn)
        {
            std::lock_guard lock(movesMutex);
            keyedMoves.push_back(std::move(move));
        }
        return actor != before;
    }

    std::vector<KeyedMove> TakeKeyedMoves()
    {
        std::lock_guard lock(movesMutex);
        return std::exchange(keyedMoves, {});
    }

    std::optional<std::uint32_t> FindActor(Types::KeyframeablePropertyType type)
    {
        const auto description = RuntimeProperties::Find(type);
        if (!description || !RuntimeProperties::IsProp(type))
            return std::nullopt;
        for (const auto& actor : Actors::GetActors())
        {
            if (!actor.placedProp)
                continue;
            for (const auto channel : PropLogic::ALL_CHANNELS)
                if (PropLogic::GetPropertyId(actor.placedProp->id, channel) == description->id)
                    return actor.id;
        }
        return std::nullopt;
    }

    std::optional<std::string> GetName(Types::KeyframeablePropertyType type)
    {
        const auto description = RuntimeProperties::Find(type);
        const auto id = FindActor(type);
        const auto actor = id ? Actors::GetActor(*id) : std::nullopt;
        if (!actor)
            return std::nullopt;
        for (const auto channel : PropLogic::ALL_CHANNELS)
            if (PropLogic::GetPropertyId(actor->placedProp->id, channel) == description->id)
                return std::format("{}: {}", actor->name, PropLogic::GetChannelName(channel));
        return std::nullopt;
    }

    void ClearKeys(std::uint32_t prop)
    {
        auto& keyframeManager = KeyframeManager::Get();
        for (const auto channel : PropLogic::ALL_CHANNELS)
        {
            const auto type = FindProperty(prop, channel);
            if (type && keyframeManager.HasProperty(*type))
                keyframeManager.ClearKeyframes(keyframeManager.GetProperty(*type));
        }
    }

    void SetPreview(const Preview& shown)
    {
        std::lock_guard lock(previewMutex);
        preview = shown;
        previewFrame = frame;
    }

    std::optional<Preview> GetPreview()
    {
        std::lock_guard lock(previewMutex);
        if (!preview || frame > previewFrame + 1)
            return std::nullopt;
        return preview;
    }
}  // namespace IWXMVM::Components::Props
