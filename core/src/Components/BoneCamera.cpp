#include "StdInclude.hpp"
#include "BoneCamera.hpp"

#include "Mod.hpp"
#include "Events.hpp"
#include "Components/Actors.hpp"
#include "Components/BoneCameraLogic.hpp"
#include "Components/CameraManager.hpp"
#include "Components/FreeCamera.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/KeyframeSerializer.hpp"
#include "Components/KillFinder.hpp"
#include "Components/Playback.hpp"
#include "Components/UndoHistory.hpp"
#include "UI/UIManager.hpp"

namespace IWXMVM::Components
{
    namespace
    {
        // what changed the settings since the frame before goes into one step of the undo history (see TrackEdit)
        BoneCameraLogic::Edit edit;
        Types::BoneCameraSettings lastSettings;
        // the camera was flown in the last frame (the view had the keyboard and mouse): a flight is one step
        bool flown = false;

        // A flight that moved keyed fields: an open group of the undo history, so that the change of their keys and of
        // the other fields is one step, and the key of each property it keys (the one at the playhead when it began),
        // by its id
        struct FlightKeys
        {
            std::map<Types::KeyframeablePropertyType, std::int32_t> keys;
        };
        std::optional<FlightKeys> flightKeys;

        std::vector<Types::Keyframe>& GetKeys(Types::KeyframeablePropertyType type)
        {
            auto& keyframeManager = KeyframeManager::Get();
            return keyframeManager.GetKeyframes(keyframeManager.GetProperty(type));
        }
    }  // namespace

    void BoneCamera::Initialize()
    {
        // what a demo's keyframes file has replaces these once it is read (see KeyframeSerializer::Read)
        Events::RegisterListener(EventType::PostDemoLoad, [&]() {
            settings = {};
            lastSettings = settings;
            edit = {};
            // (the demo's keys are gone)
            if (std::exchange(flightKeys, std::nullopt))
                UndoHistory::Get().EndGroup();
        });

        // every frame, so that an edit ends even after the camera was changed
        Events::RegisterListener(EventType::OnFrame, [&]() {
            // keyed fields follow the timeline, for the tab and the editor's camera (the game's thread applies the
            // keys again at its own frame's tick, Hooks::Camera)
            if (GetKeyed().Any())
                settings = BoneCameraLogic::ApplyKeys(settings, CopyKeys(), Playback::GetTimelineTick());

            const bool active = CameraManager::Get().GetActiveCamera()->GetMode() == Camera::Mode::Bone;
            const bool editing =
                ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left) || (active && flown);
            TrackEdit(lastSettings, editing);
            if (!editing)
                EndFlight();
            flown = false;
        });

        // a key added on the timeline to one of its rows without keys takes the row's value
        KeyframeManager::Get().AddUnkeyedValues(
            [this](Types::KeyframeablePropertyType type) { return BoneCameraLogic::GetValue(settings, type); });
    }

    BoneCameraLogic::Keys BoneCamera::CopyKeys()
    {
        using Type = Types::KeyframeablePropertyType;
        return {GetKeys(Type::BoneCameraOffset), GetKeys(Type::BoneCameraRotation), GetKeys(Type::BoneCameraFov)};
    }

    BoneCameraLogic::Keyed BoneCamera::GetKeyed()
    {
        using Type = Types::KeyframeablePropertyType;
        return {!GetKeys(Type::BoneCameraOffset).empty(), !GetKeys(Type::BoneCameraRotation).empty(),
                !GetKeys(Type::BoneCameraFov).empty()};
    }

    void BoneCamera::ResetOffsets()
    {
        const auto before = settings;
        auto reset = settings;
        const Types::BoneCameraSettings defaults;
        reset.positionOffset = defaults.positionOffset;
        reset.rotationOffset = defaults.rotationOffset;
        reset.fov = defaults.fov;

        // (those with keys keyed at the playhead, as a change of their rows would)
        auto& keyframeManager = KeyframeManager::Get();
        const auto tick = Playback::GetTimelineTick();
        UndoHistory::Get().BeginGroup("reset the bone camera's offsets");
        for (const auto type : BoneCameraLogic::KEYED_PROPERTIES)
        {
            const auto& property = keyframeManager.GetProperty(type);
            auto& keys = keyframeManager.GetKeyframes(property);
            if (keys.empty())
                continue;
            const auto value = *BoneCameraLogic::GetValue(reset, type);
            if (const auto here = std::ranges::find(keys, tick, &Types::Keyframe::tick); here != keys.end())
            {
                auto changed = *here;
                changed.value = value;
                keyframeManager.ChangeKeyframes(property, {changed}, "reset the bone camera's offsets");
            }
            else
            {
                keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, value));
            }
            keyframeManager.SortAndSaveKeyframes(keys);
        }
        settings = reset;
        TrackEdit(before, false);
        UndoHistory::Get().EndGroup();
    }

    void BoneCamera::KeyFlight(const Types::BoneCameraSettings& flownTo)
    {
        auto& keyframeManager = KeyframeManager::Get();
        if (!flightKeys)
        {
            UndoHistory::Get().BeginGroup("fly the bone camera");
            flightKeys.emplace();
        }
        const auto tick = Playback::GetTimelineTick();
        for (const auto type : BoneCameraLogic::KEYED_PROPERTIES)
        {
            const auto& property = keyframeManager.GetProperty(type);
            auto& keys = keyframeManager.GetKeyframes(property);
            if (keys.empty())
                continue;
            const auto value = *BoneCameraLogic::GetValue(flownTo, type);
            const auto known = flightKeys->keys.find(type);
            auto key = known != flightKeys->keys.end() ? std::ranges::find(keys, known->second, &Types::Keyframe::id)
                                                       : keys.end();
            if (key == keys.end())
            {
                // the key at the playhead (added there when there is none), the same one the whole flight, even as
                // the demo plays on
                key = std::ranges::find(keys, tick, &Types::Keyframe::tick);
                if (key == keys.end())
                {
                    keyframeManager.AddKeyframe(property, Types::Keyframe(property, tick, value));
                    keyframeManager.SortAndSaveKeyframes(keys);
                    key = std::ranges::find(keys, tick, &Types::Keyframe::tick);
                }
                flightKeys->keys[type] = key->id;
                keyframeManager.BeginModifyingKeyframeValue(*key);
            }
            key->value = value;
            keyframeManager.SortAndSaveKeyframes(keys);
        }
    }

    void BoneCamera::EndFlight()
    {
        if (!flightKeys)
            return;
        auto& keyframeManager = KeyframeManager::Get();
        for (const auto& [type, id] : flightKeys->keys)
        {
            const auto& property = keyframeManager.GetProperty(type);
            auto& keys = keyframeManager.GetKeyframes(property);
            if (const auto key = std::ranges::find(keys, id, &Types::Keyframe::id);
                key != keys.end() && keyframeManager.IsKeyframeValueBeingModified(*key))
                keyframeManager.EndModifyingKeyframeValue(property, *key);
        }
        flightKeys.reset();
        UndoHistory::Get().EndGroup();
    }

    void BoneCamera::SetSettings(const Types::BoneCameraSettings& newSettings)
    {
        settings = newSettings;
        lastSettings = settings;
        edit = {};
    }

    void BoneCamera::TrackEdit(const Types::BoneCameraSettings& frameStart, bool editing)
    {
        const auto keyed = GetKeyed();
        edit.Track(frameStart, settings, keyed);
        lastSettings = settings;
        if (editing || !edit.IsOpen())
            return;

        const auto step = edit.End(settings, keyed);
        if (!step)
            return;
        const auto [before, after] = *step;
        auto apply = [this](const Types::BoneCameraSettings& to) {
            SetSettings(to);
            KeyframeSerializer::WriteRecentSoon();
        };
        UndoHistory::Get().Push(BoneCameraLogic::DescribeChange(before, after, DescribeTarget(after.target)),
                                [apply, before] { apply(before); }, [apply, after] { apply(after); });
        KeyframeSerializer::WriteRecentSoon();
    }

    std::string BoneCamera::DescribeTarget(const Types::BoneCameraTarget& target)
    {
        if (target.kind == Types::BoneCameraTargetKind::Actor)
        {
            const auto actor = Actors::GetActor(static_cast<std::uint32_t>(target.id));
            return actor ? actor->name : std::format("actor {} (deleted)", target.id);
        }
        const auto events = KillFinder::GetEvents();
        return events ? events->GetPlayerName(target.id) : std::format("player {}", target.id);
    }

    void BoneCamera::HandleInput(const Types::TargetBone& target)
    {
        if (!UI::UIManager::Get().GetUIComponent(UI::Component::GameView)->HasFocus())
            return;
        // (flying: the view has the keyboard and mouse until they are given back, moved or not)
        flown = true;

        const BoneCameraLogic::Transform bone{target.position, target.axes};
        const auto frame = BoneCameraLogic::MakeFrame(target.origin, target.yaw);
        const auto placed = BoneCameraLogic::Place(bone, frame, settings);
        auto position = placed.position;
        auto angles = placed.angles;
        auto fov = settings.fov;
        FreeCamera::HandleFreecamInput(position, angles, fov, GetForwardVector(), GetRightVector());

        // (the offsets are only worked out again when it moved: a level camera's would take a roll that cancels the
        // bone's, which changes nothing in the picture but would be an edit)
        if (position == placed.position && angles == placed.angles && fov == settings.fov)
            return;
        auto flownTo = settings;
        flownTo.fov = fov;
        std::tie(flownTo.positionOffset, flownTo.rotationOffset) =
            BoneCameraLogic::GetOffsets(bone, frame, settings, position, angles);
        // (the keyed fields' keys at the playhead take the flight; the others change as their rows would)
        if (GetKeyed().Any())
            KeyFlight(flownTo);
        settings = flownTo;
    }

    void BoneCamera::Update()
    {
        const auto target = Mod::GetGameInterface()->GetTargetBone(settings.target, settings.bone);
        fov = settings.fov;
        if (!target)
            return;

        HandleInput(*target);

        const auto placed = BoneCameraLogic::Place({target->position, target->axes},
                                                   BoneCameraLogic::MakeFrame(target->origin, target->yaw), settings);
        position = placed.position;
        rotation = placed.angles;
        fov = settings.fov;
    }
}  // namespace IWXMVM::Components
