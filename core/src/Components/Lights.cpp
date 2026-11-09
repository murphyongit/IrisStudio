#include "StdInclude.hpp"
#include "Lights.hpp"

#include "Actors.hpp"
#include "Events.hpp"
#include "KeyframeManager.hpp"
#include "Playback.hpp"

namespace IWXMVM::Components::Lights
{
    namespace
    {
        std::atomic<std::shared_ptr<const AllKeys>> keysNow;
        std::atomic<std::shared_ptr<const std::set<std::uint32_t>>> keyedNow;
        std::mutex movesMutex;
        std::vector<KeyedMove> keyedMoves;

        std::optional<Types::KeyframeablePropertyType> FindProperty(std::uint32_t light, LightLogic::Channel channel)
        {
            return RuntimeProperties::FindById(LightLogic::GetPropertyId(light, channel));
        }
    }  // namespace

    void Initialize()
    {
        keysNow = std::make_shared<const AllKeys>();
        keyedNow = std::make_shared<const std::set<std::uint32_t>>();

        Events::RegisterListener(EventType::OnFrame, [] {
            auto& keyframeManager = KeyframeManager::Get();
            auto all = std::make_shared<AllKeys>();
            for (const auto& actor : Actors::GetActors())
            {
                if (!actor.light)
                    continue;
                LightLogic::Keys keys;
                auto any = false;
                for (std::size_t i = 0; i < LightLogic::ALL_CHANNELS.size(); ++i)
                {
                    const auto type = FindProperty(actor.light->id, LightLogic::ALL_CHANNELS[i]);
                    if (!type || !keyframeManager.HasProperty(*type))
                        continue;
                    keys[i] = keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type));
                    any = any || !keys[i].empty();
                }
                if (any)
                    all->emplace(actor.light->id, std::move(keys));
            }
            keysNow = std::move(all);

            auto keyed = std::make_shared<std::set<std::uint32_t>>();
            for (const auto type : RuntimeProperties::GetAll())
            {
                if (!RuntimeProperties::IsLight(type) || !keyframeManager.HasProperty(type) ||
                    keyframeManager.GetKeyframes(keyframeManager.GetProperty(type)).empty())
                    continue;
                if (const auto light = LightLogic::ParseLight(RuntimeProperties::Find(type)->id))
                    keyed->insert(*light);
            }
            keyedNow = std::move(keyed);
        });

        // a key added on a timeline row without keys takes the light's value
        KeyframeManager::Get().AddUnkeyedValues(
            [](Types::KeyframeablePropertyType type) -> std::optional<Types::KeyframeValue> {
                const auto description = RuntimeProperties::Find(type);
                if (!description)
                    return std::nullopt;
                for (const auto& actor : Actors::GetActors())
                {
                    if (!actor.light)
                        continue;
                    for (const auto channel : LightLogic::ALL_CHANNELS)
                        if (LightLogic::GetPropertyId(actor.light->id, channel) == description->id)
                            return LightLogic::GetValue(actor, channel);
                }
                return std::nullopt;
            });
    }

    const Types::KeyframeableProperty& GetProperty(std::uint32_t light, LightLogic::Channel channel)
    {
        return KeyframeManager::Get().AddProperty(RuntimeProperties::Register(LightLogic::Describe(light, channel)));
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

    KeyCopy TakeKeys(std::uint32_t light)
    {
        auto& keyframeManager = KeyframeManager::Get();
        KeyCopy copy;
        for (const auto channel : LightLogic::ALL_CHANNELS)
        {
            const auto type = FindProperty(light, channel);
            if (!type || !keyframeManager.HasProperty(*type))
                continue;
            const auto& keys = keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type));
            if (!keys.empty())
                copy.emplace_back(channel, keys);
        }
        return copy;
    }

    void PutKeys(std::uint32_t light, const KeyCopy& keys, std::int64_t ticks)
    {
        auto& keyframeManager = KeyframeManager::Get();
        for (const auto& [channel, channelKeys] : keys)
        {
            const auto& property = GetProperty(light, channel);
            for (const auto& key : channelKeys)
            {
                auto copy = Types::Keyframe(property, static_cast<std::uint32_t>(std::max<std::int64_t>(key.tick + ticks, 0)),
                                            key.value);
                copy.interpolation = key.interpolation;
                copy.easeIn = key.easeIn;
                copy.easeOut = key.easeOut;
                keyframeManager.AddKeyframe(property, copy);
            }
            keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(property));
        }
    }

    std::vector<Types::KeyframeablePropertyType> GetKeyedProperties(std::uint32_t light)
    {
        auto& keyframeManager = KeyframeManager::Get();
        std::vector<Types::KeyframeablePropertyType> properties;
        for (const auto channel : LightLogic::ALL_CHANNELS)
        {
            const auto type = FindProperty(light, channel);
            if (type && keyframeManager.HasProperty(*type) &&
                !keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type)).empty())
                properties.push_back(*type);
        }
        return properties;
    }

    Types::Actor WithKeyedPlacement(const Types::Actor& actor, std::uint32_t tick)
    {
        const auto keys = GetKeys();
        if (!actor.light || !keys)
            return actor;
        const auto found = keys->find(actor.light->id);
        return found == keys->end() ? actor : LightLogic::ApplyPlacementKeys(actor, found->second, tick);
    }

    Types::Actor WithKeyedPlacementNow(const Types::Actor& actor, std::uint32_t tick)
    {
        if (!actor.light)
            return actor;
        auto& keyframeManager = KeyframeManager::Get();
        LightLogic::Keys keys;
        for (const auto channel : {LightLogic::Channel::Position, LightLogic::Channel::Aim})
        {
            const auto type = FindProperty(actor.light->id, channel);
            if (type && keyframeManager.HasProperty(*type))
                keys[static_cast<std::size_t>(channel)] =
                    keyframeManager.GetKeyframes(keyframeManager.GetProperty(*type));
        }
        return LightLogic::ApplyPlacementKeys(actor, keys, tick);
    }

    bool MoveTo(Types::Actor& actor, const Types::Actor& placed, const std::string& step)
    {
        if (!actor.light || !placed.light)
            return false;
        const auto before = actor;
        const auto all = GetKeys();
        const auto found = all ? all->find(actor.light->id) : AllKeys::const_iterator{};
        const auto keyed = [&](LightLogic::Channel channel) {
            return all && found != all->end() && !found->second[static_cast<std::size_t>(channel)].empty();
        };
        // (what it moved from: where its keys have it at the playhead)
        const auto from = WithKeyedPlacement(actor, Playback::GetTimelineTick());
        KeyedMove move{.actor = actor.id, .step = step};
        for (const auto channel : {LightLogic::Channel::Position, LightLogic::Channel::Aim})
        {
            const auto value = LightLogic::GetValue(placed, channel);
            if (value.vector3 == LightLogic::GetValue(from, channel).vector3)
                continue;
            if (keyed(channel))
                (channel == LightLogic::Channel::Position ? move.position : move.aim) = value.vector3;
            else
                LightLogic::SetValue(actor, channel, value);
        }
        // (a spot put at the camera reaches what was clicked)
        if (!keyed(LightLogic::Channel::Radius))
            actor.light->radius = placed.light->radius;
        if (move.position || move.aim)
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
        if (!description || !RuntimeProperties::IsLight(type))
            return std::nullopt;
        for (const auto& actor : Actors::GetActors())
        {
            if (!actor.light)
                continue;
            for (const auto channel : LightLogic::ALL_CHANNELS)
                if (LightLogic::GetPropertyId(actor.light->id, channel) == description->id)
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
        for (const auto channel : LightLogic::ALL_CHANNELS)
            if (LightLogic::GetPropertyId(actor->light->id, channel) == description->id)
                return std::format("{}: {}", actor->name, LightLogic::GetChannelName(channel));
        return std::nullopt;
    }

    void ClearKeys(std::uint32_t light)
    {
        auto& keyframeManager = KeyframeManager::Get();
        for (const auto channel : LightLogic::ALL_CHANNELS)
        {
            const auto type = FindProperty(light, channel);
            if (type && keyframeManager.HasProperty(*type))
                keyframeManager.ClearKeyframes(keyframeManager.GetProperty(*type));
        }
    }
}  // namespace IWXMVM::Components::Lights
