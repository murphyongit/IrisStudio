#include "StdInclude.hpp"
#include "KeyframeManager.hpp"

#include "Resources.hpp"
#include "Utilities/MathUtils.hpp"
#include "KeyframeSerializer.hpp"
#include "RuntimeProperties.hpp"
#include "SkyLogic.hpp"
#include "../UI/Components/TimelineGraph.hpp"
#include "../UI/TimelineSelection.hpp"
#include "Markers.hpp"
#include "../UI/UIManager.hpp"
#include "Components/Playback.hpp"
#include "Events.hpp"
#include "Mod.hpp"
#include "UndoHistory.hpp"
#include "Configuration/Configuration.hpp"
#include "Configuration/PreferencesConfiguration.hpp"

namespace IWXMVM::Components
{
    // what the graph calls the three values of a color, and of Black Ops' filmtweaks per range
    constexpr std::array<std::string_view, 3> COLOR = {"R", "G", "B"};
    constexpr std::array<std::string_view, 3> RANGES = {"Shadows", "Midtones", "Highlights"};

     Types::KeyframeableProperty campathCameraProperty(
        Types::KeyframeablePropertyType::CampathCamera, 
        ICON_FA_VIDEO " Campath Camera",
        Types::KeyframeValueType::CameraData,
        -50, 50 // TODO: this is not super helpful for rotation and fov
    );
    Types::KeyframeableProperty sunLightColorProperty(
        Types::KeyframeablePropertyType::SunLightColor,
        ICON_FA_SUN " Sun Light Color",
        Types::KeyframeValueType::Vector3,
        0, 1, COLOR
    );
    Types::KeyframeableProperty sunLightBrightnessProperty(
        Types::KeyframeablePropertyType::SunLightBrightness,
        ICON_FA_SUN " Sun Light Brightness",
        Types::KeyframeValueType::FloatingPoint,
        0, 4
    );
    Types::KeyframeableProperty sunLightDirectionProperty(
        Types::KeyframeablePropertyType::SunLightDirection,
        ICON_FA_SUN " Sun Light Direction",
        Types::KeyframeValueType::Vector3,
        -360, 360
    );
    Types::KeyframeableProperty filmtweakBrightnessProperty(
        Types::KeyframeablePropertyType::FilmtweakBrightness,
        ICON_FA_IMAGE " Filmtweak Brightness",
        Types::KeyframeValueType::FloatingPoint,
        -1, 1
    );
    Types::KeyframeableProperty filmtweakContrastProperty(
        Types::KeyframeablePropertyType::FilmtweakContrast,
        ICON_FA_IMAGE " Filmtweak Contrast",
        Types::KeyframeValueType::FloatingPoint,
        0, 4
    );
    Types::KeyframeableProperty filmtweakDesaturationProperty(
        Types::KeyframeablePropertyType::FilmtweakDesaturation,
        ICON_FA_IMAGE " Filmtweak Desaturation",
        Types::KeyframeValueType::FloatingPoint, 
        0, 1
    );
    Types::KeyframeableProperty filmtweakTintLightProperty(
        Types::KeyframeablePropertyType::FilmtweakTintLight,
        ICON_FA_IMAGE " Filmtweak Tint Light",
        Types::KeyframeValueType::Vector3, 
        0, 1, COLOR
    );
    Types::KeyframeableProperty filmtweakTintDarkProperty(
        Types::KeyframeablePropertyType::FilmtweakTintDark,
        ICON_FA_IMAGE " Filmtweak Tint Dark",
        Types::KeyframeValueType::Vector3, 
        0, 1, COLOR
    );
    Types::KeyframeableProperty dofFarBlur(
        Types::KeyframeablePropertyType::DepthOfFieldFarBlur,
        ICON_FA_CAMERA " DOF Far Blur",
        Types::KeyframeValueType::FloatingPoint,
        0, 10
    );
    Types::KeyframeableProperty dofFarStart(
        Types::KeyframeablePropertyType::DepthOfFieldFarStart,
        ICON_FA_CAMERA " DOF Far Start",
        Types::KeyframeValueType::FloatingPoint, 
        0, 5000
    );
    Types::KeyframeableProperty dofFarEnd(
        Types::KeyframeablePropertyType::DepthOfFieldFarEnd,
        ICON_FA_CAMERA " DOF Far End", 
        Types::KeyframeValueType::FloatingPoint, 
        0, 5000
    );
    Types::KeyframeableProperty dofNearBlur(
        Types::KeyframeablePropertyType::DepthOfFieldNearBlur,
        ICON_FA_CAMERA " DOF Near Blur", 
        Types::KeyframeValueType::FloatingPoint,
        0, 10
    );
    Types::KeyframeableProperty dofNearStart(
        Types::KeyframeablePropertyType::DepthOfFieldNearStart,
        ICON_FA_CAMERA " DOF Near Start", 
        Types::KeyframeValueType::FloatingPoint, 
        0, 5000
    );
    Types::KeyframeableProperty dofNearEnd(
        Types::KeyframeablePropertyType::DepthOfFieldNearEnd,
        ICON_FA_CAMERA " DOF Near End", 
        Types::KeyframeValueType::FloatingPoint,
        0, 5000
    );
    Types::KeyframeableProperty dofBias(
        Types::KeyframeablePropertyType::DepthOfFieldBias,
        ICON_FA_CAMERA " DOF Bias", 
        Types::KeyframeValueType::FloatingPoint, 
        0.1f, 10
    );

    // (switches: 1 on, 0 off, held until the next key)
    Types::KeyframeableProperty filmtweakEnabledProperty(Types::KeyframeablePropertyType::FilmtweakEnabled,
                                                         ICON_FA_IMAGE " Filmtweak Enabled",
                                                         Types::KeyframeValueType::FloatingPoint, 0, 1);
    Types::KeyframeableProperty filmtweakInvertProperty(Types::KeyframeablePropertyType::FilmtweakInvert,
                                                        ICON_FA_IMAGE " Filmtweak Invert",
                                                        Types::KeyframeValueType::FloatingPoint, 0, 1);
    Types::KeyframeableProperty dofEnabled(Types::KeyframeablePropertyType::DepthOfFieldEnabled,
                                           ICON_FA_CAMERA " DOF Enabled", Types::KeyframeValueType::FloatingPoint, 0,
                                           1);

    // Black Ops' filmtweaks per range, the dark, mid and light range's in one key (the limits are its dvars')
    Types::KeyframeableProperty filmtweakContrastRangesProperty(
        Types::KeyframeablePropertyType::FilmtweakContrastRanges, ICON_FA_IMAGE " Filmtweak Contrast Ranges",
        Types::KeyframeValueType::Vector3, 0, 2, RANGES);
    Types::KeyframeableProperty filmtweakSaturationRangesProperty(
        Types::KeyframeablePropertyType::FilmtweakSaturationRanges, ICON_FA_IMAGE " Filmtweak Saturation Ranges",
        Types::KeyframeValueType::Vector3, 0, 2, RANGES);
    Types::KeyframeableProperty filmtweakHueRangesProperty(Types::KeyframeablePropertyType::FilmtweakHueRanges,
                                                           ICON_FA_IMAGE " Filmtweak Hue Ranges",
                                                           Types::KeyframeValueType::Vector3, -180, 180, RANGES);
    Types::KeyframeableProperty filmtweakTemperatureRangesProperty(
        Types::KeyframeablePropertyType::FilmtweakTemperatureRanges, ICON_FA_IMAGE " Filmtweak Temperature Ranges",
        Types::KeyframeValueType::Vector3, 1650, 25000, RANGES);
    Types::KeyframeableProperty filmtweakBleachRangesProperty(Types::KeyframeablePropertyType::FilmtweakBleachRanges,
                                                              ICON_FA_IMAGE " Filmtweak Bleach Ranges",
                                                              Types::KeyframeValueType::Vector3, 0, 1, RANGES);
    Types::KeyframeableProperty filmtweakTintMidProperty(Types::KeyframeablePropertyType::FilmtweakTintMid,
                                                         ICON_FA_IMAGE " Filmtweak Tint Mid",
                                                         Types::KeyframeValueType::Vector3, 0, 6, COLOR);
    Types::KeyframeableProperty filmtweakMidStartProperty(Types::KeyframeablePropertyType::FilmtweakMidStart,
                                                          ICON_FA_IMAGE " Filmtweak Mid Start",
                                                          Types::KeyframeValueType::FloatingPoint, 0, 1);
    Types::KeyframeableProperty filmtweakMidEndProperty(Types::KeyframeablePropertyType::FilmtweakMidEnd,
                                                        ICON_FA_IMAGE " Filmtweak Mid End",
                                                        Types::KeyframeValueType::FloatingPoint, 0, 1);
    Types::KeyframeableProperty filmtweakDarkFeatherProperty(Types::KeyframeablePropertyType::FilmtweakDarkFeather,
                                                             ICON_FA_IMAGE " Filmtweak Dark Feather",
                                                             Types::KeyframeValueType::FloatingPoint, 0, 1);
    Types::KeyframeableProperty filmtweakLightFeatherProperty(Types::KeyframeablePropertyType::FilmtweakLightFeather,
                                                              ICON_FA_IMAGE " Filmtweak Light Feather",
                                                              Types::KeyframeValueType::FloatingPoint, 0, 1);

    // (the slider's range, Components::QualityLogic)
    Types::KeyframeableProperty shadowDistanceProperty(Types::KeyframeablePropertyType::ShadowDistance,
                                                       ICON_FA_SUN " Shadow Distance",
                                                       Types::KeyframeValueType::FloatingPoint, 1100, 10000);
    // (the sliders' ranges, Components::SurfaceLogic)
    Types::KeyframeableProperty surfaceDepthProperty(Types::KeyframeablePropertyType::SurfaceDepth,
                                                     ICON_FA_LAYER_GROUP " Surface Depth",
                                                     Types::KeyframeValueType::FloatingPoint, 0, 4);
    Types::KeyframeableProperty bumpStrengthProperty(Types::KeyframeablePropertyType::BumpStrength,
                                                     ICON_FA_LAYER_GROUP " Bump Strength",
                                                     Types::KeyframeValueType::FloatingPoint, 0.5f, 3);

    // the bone camera's: its offset in the bone's space, its turn from the bone's axes and
    // its field of view, the Camera tab's rows
    Types::KeyframeableProperty boneCameraOffsetProperty(Types::KeyframeablePropertyType::BoneCameraOffset,
                                                         ICON_FA_BONE " Bone Camera Offset",
                                                         Types::KeyframeValueType::Vector3, -100, 100,
                                                         {"Forward", "Left", "Up"});
    Types::KeyframeableProperty boneCameraRotationProperty(Types::KeyframeablePropertyType::BoneCameraRotation,
                                                           ICON_FA_BONE " Bone Camera Rotation",
                                                           Types::KeyframeValueType::Vector3, -180, 180,
                                                           {"Pitch", "Yaw", "Roll"});
    Types::KeyframeableProperty boneCameraFovProperty(Types::KeyframeablePropertyType::BoneCameraFov,
                                                      ICON_FA_BONE " Bone Camera FOV",
                                                      Types::KeyframeValueType::FloatingPoint, 1, 160);

    // the Look tab's Sky (the sliders' ranges, Components::SkyLogic)
    Types::KeyframeableProperty skyHueProperty(Types::KeyframeablePropertyType::SkyHue, ICON_FA_CLOUD_SUN " Sky Hue",
                                               Types::KeyframeValueType::FloatingPoint, Components::SkyLogic::MIN_HUE,
                                               Components::SkyLogic::MAX_HUE);
    Types::KeyframeableProperty skySaturationProperty(Types::KeyframeablePropertyType::SkySaturation,
                                                      ICON_FA_CLOUD_SUN " Sky Saturation",
                                                      Types::KeyframeValueType::FloatingPoint, 0,
                                                      Components::SkyLogic::MAX_SATURATION);
    Types::KeyframeableProperty skyBrightnessProperty(Types::KeyframeablePropertyType::SkyBrightness,
                                                      ICON_FA_CLOUD_SUN " Sky Brightness",
                                                      Types::KeyframeValueType::FloatingPoint, 0,
                                                      Components::SkyLogic::MAX_BRIGHTNESS);
    Types::KeyframeableProperty skyContrastProperty(Types::KeyframeablePropertyType::SkyContrast,
                                                    ICON_FA_CLOUD_SUN " Sky Contrast",
                                                    Types::KeyframeValueType::FloatingPoint, 0,
                                                    Components::SkyLogic::MAX_CONTRAST);
    Types::KeyframeableProperty skyTintProperty(Types::KeyframeablePropertyType::SkyTint, ICON_FA_CLOUD_SUN " Sky Tint",
                                                Types::KeyframeValueType::Vector3, 0, Components::SkyLogic::MAX_TINT,
                                                {"R", "G", "B"});
    Types::KeyframeableProperty skyRotationProperty(Types::KeyframeablePropertyType::SkyRotation,
                                                    ICON_FA_CLOUD_SUN " Sky Rotation",
                                                    Types::KeyframeValueType::Vector3,
                                                    -Components::SkyLogic::MAX_ROTATION,
                                                    Components::SkyLogic::MAX_ROTATION, {"Yaw", "Pitch", "Roll"});

    void KeyframeManager::Initialize()
    {
        auto InitializeProperty = [&](auto property) {
            keyframes[property] = std::vector<Types::Keyframe>();
            byType[property.type] = &keyframes.find(property)->first;
        };
        
        InitializeProperty(campathCameraProperty);
        InitializeProperty(sunLightColorProperty);
        InitializeProperty(sunLightBrightnessProperty);
        InitializeProperty(sunLightDirectionProperty);
        InitializeProperty(filmtweakBrightnessProperty);
        InitializeProperty(filmtweakContrastProperty);
        InitializeProperty(filmtweakDesaturationProperty);
        InitializeProperty(filmtweakTintLightProperty);
        InitializeProperty(filmtweakTintDarkProperty);
        InitializeProperty(dofFarBlur);
        InitializeProperty(dofFarStart);
        InitializeProperty(dofFarEnd);
        InitializeProperty(dofNearBlur);
        InitializeProperty(dofNearStart);
        InitializeProperty(dofNearEnd);
        InitializeProperty(dofBias);
        InitializeProperty(filmtweakEnabledProperty);
        InitializeProperty(filmtweakInvertProperty);
        InitializeProperty(dofEnabled);
        InitializeProperty(filmtweakContrastRangesProperty);
        InitializeProperty(filmtweakSaturationRangesProperty);
        InitializeProperty(filmtweakHueRangesProperty);
        InitializeProperty(filmtweakTemperatureRangesProperty);
        InitializeProperty(filmtweakBleachRangesProperty);
        InitializeProperty(filmtweakTintMidProperty);
        InitializeProperty(filmtweakMidStartProperty);
        InitializeProperty(filmtweakMidEndProperty);
        InitializeProperty(filmtweakDarkFeatherProperty);
        InitializeProperty(filmtweakLightFeatherProperty);
        InitializeProperty(shadowDistanceProperty);
        InitializeProperty(surfaceDepthProperty);
        InitializeProperty(bumpStrengthProperty);
        InitializeProperty(boneCameraOffsetProperty);
        InitializeProperty(boneCameraRotationProperty);
        InitializeProperty(boneCameraFovProperty);
        InitializeProperty(skyHueProperty);
        InitializeProperty(skySaturationProperty);
        InitializeProperty(skyBrightnessProperty);
        InitializeProperty(skyContrastProperty);
        InitializeProperty(skyTintProperty);
        InitializeProperty(skyRotationProperty);

        static bool justLoadedDemo = false;

        // what is left to save goes with the demo it belongs to
        Events::RegisterListener(EventType::PreDemoLoad,
                                 [] { Components::KeyframeSerializer::WriteRecentIfChanged(true); });

        Events::RegisterListener(EventType::PostDemoLoad, [&]() { 
            
            ClearKeyframes();
            
            if (Components::Playback::IsGameFrozen())
            {
                Components::Playback::ToggleFrozenTick();
            }

            // (what was done in the demo before can't be undone in this one, and its markers and selection are its own)
            UI::TimelineSelection::Clear();
            UI::TimelineSelection::ForgetCopied();
            UndoHistory::Get().Clear();
            Markers::Clear();
            Components::Playback::SetFrozenSpans({});
            constantSpeed = false;
            justLoadedDemo = true;
        });

        Events::RegisterListener(EventType::OnFrame, [&]() {
            Components::KeyframeSerializer::WriteRecentIfChanged();

            if (IWXMVM::Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo && justLoadedDemo)
            {
                Components::KeyframeSerializer::ReadRecent();
                justLoadedDemo = false;
                UI::TimelineGraph::Refit();
            }
        });
    }

    const Types::KeyframeableProperty& KeyframeManager::GetProperty(const Types::KeyframeablePropertyType property) const
    {
        if (const auto found = byType.find(property); found != byType.end())
            return *found->second;

        throw std::runtime_error("Unregistered keyframeable property type");
    }

    const Types::KeyframeableProperty& KeyframeManager::AddProperty(Types::KeyframeablePropertyType runtimeType)
    {
        if (const auto found = byType.find(runtimeType); found != byType.end())
            return *found->second;
        const auto [added, _] = keyframes.try_emplace(RuntimeProperties::MakeProperty(runtimeType));
        byType[runtimeType] = &added->first;
        return added->first;
    }

    std::string KeyframeManager::GetTypeName(Types::KeyframeablePropertyType type)
    {
        if (const auto runtime = RuntimeProperties::Find(type))
            return (RuntimeProperties::IsLight(type)  ? "Light:"
                    : RuntimeProperties::IsProp(type) ? "Prop:"
                                                      : "ReShade:") +
                   runtime->id;
        return std::string(magic_enum::enum_name(type));
    }

    void KeyframeManager::ClearKeyframes()
    {
        UndoHistory::Get().BeginGroup("clear all keyframes");
        for (auto& [p, k] : keyframes)
        {
            ClearKeyframes(p);
        }
        UndoHistory::Get().EndGroup();
        // (a drag of a key that is gone now ends with it)
        beginningTickMap.clear();
        beginningValueMap.clear();
    }

    void KeyframeManager::ClearKeyframes(Types::KeyframeableProperty property)
    {
        RemoveKeyframes(property, keyframes[property],
                        property.type == Types::KeyframeablePropertyType::CampathCamera ? "clear campath"
                                                                                       : "clear keys");
    }

    std::string KeyframeManager::DescribeKeys(const Types::KeyframeableProperty& property, std::size_t count)
    {
        const bool node = property.type == Types::KeyframeablePropertyType::CampathCamera;
        if (count == 1)
            return node ? "node" : "key";
        return std::format("{} {}", count, node ? "nodes" : "keys");
    }

    bool KeyframeManager::AreKeyframesBeingModified()
    {
        return !beginningTickMap.empty() || !beginningValueMap.empty();
    }

    void KeyframeManager::SortAndSaveKeyframes(std::vector<Types::Keyframe>& keyframes)
    {
        std::sort(keyframes.begin(), keyframes.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
        // (the campath's ends as they are now)
        if (&keyframes == &this->keyframes[GetProperty(Types::KeyframeablePropertyType::CampathCamera)])
            ApplyAutoEase();

        Components::KeyframeSerializer::WriteRecentSoon();
    }

    KeyframeManager::AutoEase KeyframeManager::GetAutoEase() const
    {
        const auto& preferences = PreferencesConfiguration::Get();
        return {preferences.autoEase, preferences.autoEaseStart, preferences.autoEaseEnd, preferences.autoEaseNodes};
    }

    void KeyframeManager::SetAutoEase(const AutoEase& autoEase, std::string label, bool ended)
    {
        const auto set = [](const AutoEase& to, bool save) {
            auto& preferences = PreferencesConfiguration::Get();
            preferences.autoEase = to.on;
            preferences.autoEaseStart = std::clamp(to.start, 0.0f, 1.0f);
            preferences.autoEaseEnd = std::clamp(to.end, 0.0f, 1.0f);
            preferences.autoEaseNodes = to.nodes;
            KeyframeManager::Get().ApplyAutoEase();
            if (save)
                Configuration::Get().Write(true);
        };
        const auto before = GetAutoEase();
        if (before == autoEase)
        {
            if (ended)
                Configuration::Get().Write(true);
            return;
        }
        set(autoEase, ended);
        UndoHistory::Get().Push(
            std::move(label), [set, before] { set(before, true); }, [set, autoEase] { set(autoEase, true); });
    }

    void KeyframeManager::ApplyAutoEase()
    {
        const auto autoEase = GetAutoEase();
        if (!autoEase.on)
            return;
        auto& nodes = keyframes[GetProperty(Types::KeyframeablePropertyType::CampathCamera)];
        if (KeyframeLogic::ApplyAutoEase(nodes, autoEase.start, autoEase.end, autoEase.nodes))
            Components::KeyframeSerializer::WriteRecentSoon();
    }

    void KeyframeManager::AddKeyframe(Types::KeyframeableProperty property, Types::Keyframe keyframeToAdd)
    {
        keyframeToAdd.interpolation =
            KeyframeLogic::GetNewKeyInterpolation(keyframes[property], keyframeToAdd.tick, property.type);
        // (a campath node made with Auto Ease on follows it)
        if (property.type == Types::KeyframeablePropertyType::CampathCamera)
            keyframeToAdd.autoEaseIn = keyframeToAdd.autoEaseOut = GetAutoEase().on;
        AddKeyframes(property, std::vector<Types::Keyframe>{keyframeToAdd});
    }

    void KeyframeManager::ChangeKeyframes(const Types::KeyframeableProperty& property,
                                          std::vector<Types::Keyframe> changed, std::string label)
    {
        std::vector<Types::Keyframe> before, after;
        auto& keys = keyframes[property];
        for (auto& key : changed)
        {
            const auto it = std::ranges::find(keys, key.id, &Types::Keyframe::id);
            if (it == keys.end())
                continue;
            before.push_back(*it);
            // (a switch's keys stay Hold; a node's ease changed by hand is its own, see AutoEase)
            after.push_back(KeyframeLogic::Normalize(key, property.type));
            if (property.type == Types::KeyframeablePropertyType::CampathCamera)
                KeyframeLogic::KeepChangedEases(*it, after.back());
        }
        if (after.empty())
            return;
        const auto action = std::make_shared<ChangeKeyframesAction>(property, std::move(before), std::move(after));
        action->DoAction();
        SortAndSaveKeyframes(keys);
        AddActionToHistory(action, std::move(label));
    }

    void KeyframeManager::EndNodeDrag(const KeyframeLogic::KeyDrag::Taken& drag)
    {
        const auto& property = GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        auto& nodes = keyframes[property];
        const auto node = std::ranges::find(nodes, drag.id, &Types::Keyframe::id);
        if (node == nodes.end())
            return;
        const auto& before = drag.before.cameraData;
        const auto& after = node->value.cameraData;
        // (with the angles it had: the gizmo may have written the same turn another way round)
        if (KeyframeLogic::IsSameCamera(before, after))
        {
            node->value = drag.before;
            return;
        }
        auto label = std::format("{} {}", KeyframeLogic::GetNodeDragVerb(before, after), DescribeKeys(property, 1));
        AddActionToHistory(std::make_shared<ModifyValueAction>(property, drag.before, node->value, node->id),
                           std::move(label));
        SortAndSaveKeyframes(nodes);
    }

    void KeyframeManager::SetConstantSpeed(bool on)
    {
        if (constantSpeed == on)
            return;
        constantSpeed = on;
        Components::KeyframeSerializer::WriteRecentSoon();
        const auto set = [](bool value) {
            KeyframeManager::Get().constantSpeed = value;
            Components::KeyframeSerializer::WriteRecentSoon();
        };
        UndoHistory::Get().Push(
            on ? "constant speed" : "speed by the nodes' times", [set, on] { set(!on); }, [set, on] { set(on); });
    }

    namespace
    {
        // whether two lists of nodes make the same campath
        bool SameNodes(const std::vector<Types::Keyframe>& a, const std::vector<Types::Keyframe>& b)
        {
            return std::ranges::equal(a, b, [](const Types::Keyframe& x, const Types::Keyframe& y) {
                return x.tick == y.tick && x.interpolation == y.interpolation && x.easeIn == y.easeIn &&
                       x.easeOut == y.easeOut && std::memcmp(&x.value, &y.value, sizeof(Types::KeyframeValue)) == 0;
            });
        }
    }  // namespace

    std::shared_ptr<const KeyframeLogic::Retiming> KeyframeManager::GetRetiming(
        const std::vector<Types::Keyframe>& nodes) const
    {
        std::lock_guard lock(retimingMutex);
        for (const auto& [seen, retiming] : retimings)
        {
            if (SameNodes(seen, nodes))
                return retiming;
        }
        // (the UI's nodes and the game thread's copy of them, which differ for a frame while they change)
        auto retiming = std::make_shared<const KeyframeLogic::Retiming>(KeyframeLogic::MakeRetiming(nodes));
        retimings.emplace_back(nodes, retiming);
        if (retimings.size() > 2)
            retimings.erase(retimings.begin());
        return retiming;
    }

    double KeyframeManager::GetCampathPassingTick(double pathTick) const
    {
        const auto& nodes = keyframes.at(GetProperty(Types::KeyframeablePropertyType::CampathCamera));
        if (!constantSpeed || nodes.size() < 2)
            return pathTick;
        return KeyframeLogic::GetPassingTick(*GetRetiming(nodes), pathTick);
    }

    double KeyframeManager::GetCampathLength() const
    {
        const auto& nodes = keyframes.at(GetProperty(Types::KeyframeablePropertyType::CampathCamera));
        return nodes.size() < 2 ? 0.0 : GetRetiming(nodes)->GetLength();
    }

    void KeyframeManager::AddKeyframes(Types::KeyframeableProperty property,
                                       const std::vector<Types::Keyframe> keyframesToAdd)
    {
        std::vector<Types::Keyframe> added;
        for (const auto& key : keyframesToAdd)
            added.push_back(KeyframeLogic::Normalize(key, property.type));
        std::shared_ptr<AddKeyframesAction> addAction = std::make_shared<AddKeyframesAction>(property, added);
        addAction->DoAction();
        AddActionToHistory(addAction, "add " + DescribeKeys(property, keyframesToAdd.size()));
        if (property.type == Types::KeyframeablePropertyType::CampathCamera)
            SortAndSaveKeyframes(keyframes[property]);
    }

    void KeyframeManager::RemoveKeyframe(Types::KeyframeableProperty property,
                                         std::vector<Types::Keyframe>::iterator it)
    {
        RemoveKeyframes(property, {*it});
    }

    void KeyframeManager::RemoveKeyframe(Types::KeyframeableProperty property, size_t indexToRemove)
    {
        RemoveKeyframes(property, {keyframes[property].at(indexToRemove)});
    }

    void KeyframeManager::RemoveKeyframes(Types::KeyframeableProperty property,
                                          std::vector<Types::Keyframe> keyframesToRemove, std::string label)
    {
        if (!keyframes[property].empty() && !keyframesToRemove.empty())
        {
            if (label.empty())
                label = "delete " + DescribeKeys(property, keyframesToRemove.size());
            std::shared_ptr<RemoveKeyframesAction> removeAction =
                std::make_shared<RemoveKeyframesAction>(property, keyframesToRemove);
            removeAction->DoAction();
            AddActionToHistory(removeAction, std::move(label));
            if (property.type == Types::KeyframeablePropertyType::CampathCamera)
                SortAndSaveKeyframes(keyframes[property]);
        }
    }

    bool KeyframeManager::IsKeyframeTickBeingModified(Types::Keyframe& keyframe)
    {
        return beginningTickMap.find(keyframe.id) != beginningTickMap.end();
    }

    void KeyframeManager::BeginModifyingKeyframeTick(Types::Keyframe& keyframeToModify)
    {
        LOG_DEBUG("Begin Modifying Tick " + std::to_string(keyframeToModify.id));
        beginningTickMap[keyframeToModify.id] = keyframeToModify.tick;
    }

    void KeyframeManager::EndModifyingKeyframeTick(Types::KeyframeableProperty property,
                                                   Types::Keyframe& keyframeToModify)
    {
        LOG_DEBUG("End Modifying Tick " + std::to_string(keyframeToModify.id));
        std::shared_ptr<ModifyTickAction> modifyAction = std::make_shared<ModifyTickAction>(
            property, beginningTickMap[keyframeToModify.id], keyframeToModify.tick, keyframeToModify.id);
        // (a click on a key that didn't move it changes nothing)
        if (modifyAction->oldTick != modifyAction->newTick)
            AddActionToHistory(modifyAction, "move " + DescribeKeys(property, 1));
        beginningTickMap.erase(keyframeToModify.id);
    }

    bool KeyframeManager::IsKeyframeValueBeingModified(Types::Keyframe& keyframe)
    {
        return beginningValueMap.find(keyframe.id) != beginningValueMap.end();
    }

    void KeyframeManager::BeginModifyingKeyframeValue(Types::Keyframe& keyframeToModify)
    {
        LOG_DEBUG("Begin Modifying Value " + std::to_string(keyframeToModify.id));
        beginningValueMap[keyframeToModify.id] = keyframeToModify.value;
    }

    void KeyframeManager::EndModifyingKeyframeValue(Types::KeyframeableProperty property,
                                                   Types::Keyframe& keyframeToModify)
    {
        LOG_DEBUG("End Modifying Value " + std::to_string(keyframeToModify.id));
        std::shared_ptr<ModifyValueAction> modifyAction = std::make_shared<ModifyValueAction>(
            property, beginningValueMap[keyframeToModify.id], keyframeToModify.value, keyframeToModify.id);
        if (std::memcmp(&modifyAction->oldValue, &modifyAction->newValue, sizeof(Types::KeyframeValue)) != 0)
            AddActionToHistory(modifyAction, "change " + DescribeKeys(property, 1));
        beginningValueMap.erase(keyframeToModify.id);
    }

    void KeyframeManager::EndModifyingKeyframeTickAndValue(Types::KeyframeableProperty property,
                                                    Types::Keyframe& keyframeToModify)
    {
        LOG_DEBUG("End Modifying Tick & Value " + std::to_string(keyframeToModify.id));
        
        std::shared_ptr<ModifyTickAndValueAction> modifyAction = std::make_shared<ModifyTickAndValueAction>(
            property, beginningTickMap[keyframeToModify.id], keyframeToModify.tick,
                                                       beginningValueMap[keyframeToModify.id],keyframeToModify.value, keyframeToModify.id);
        if (modifyAction->oldTick != modifyAction->newTick ||
            std::memcmp(&modifyAction->oldValue, &modifyAction->newValue, sizeof(Types::KeyframeValue)) != 0)
            AddActionToHistory(modifyAction, "move " + DescribeKeys(property, 1));
        beginningTickMap.erase(keyframeToModify.id);
        beginningValueMap.erase(keyframeToModify.id);
    }

    Types::KeyframeValue KeyframeManager::Interpolate(const Types::KeyframeableProperty& property,
                                                      const std::vector<Types::Keyframe>& keyframes,
                                                      const float tick) const
    {
        if (keyframes.empty())
            return Types::KeyframeValue::GetDefaultValue(property.valueType);

        if (property.type == Types::KeyframeablePropertyType::CampathCamera && constantSpeed && keyframes.size() >= 2)
            return KeyframeLogic::EvaluateRetimed(*GetRetiming(keyframes), keyframes, tick);
        return KeyframeLogic::Evaluate(property.valueType, keyframes, tick);
    }

    Types::KeyframeValue KeyframeManager::Interpolate(const Types::KeyframeableProperty& property,
                                                      const std::vector<Types::Keyframe>& keyframes,
                                                      const uint32_t tick) const 
    {
        return Interpolate(property, keyframes, static_cast<float>(tick));
    }

    Types::KeyframeValue KeyframeManager::Interpolate(const Types::KeyframeableProperty& property, const float tick) const
    {
        const auto& keyframes = KeyframeManager::Get().GetKeyframes(property);
        return Interpolate(property, keyframes, tick);
    }

    Types::KeyframeValue KeyframeManager::Interpolate(const Types::KeyframeableProperty& property,
                                                      const uint32_t tick) const
    {
        return Interpolate(property, static_cast<float>(tick));
    }

    Types::KeyframeValue KeyframeManager::GetValueAt(const Types::KeyframeableProperty& property,
                                                     std::uint32_t tick) const
    {
        const auto& keys = keyframes.at(property);
        if (keys.empty())
        {
            for (const auto& source : unkeyedValues)
            {
                if (const auto value = source(property.type))
                    return *value;
            }
        }
        return Interpolate(property, keys, tick);
    }

    void KeyframeManager::AddActionToHistory(std::shared_ptr<KeyframeAction> action, std::string label)
    {
        auto& keyframeManager = KeyframeManager::Get();
        UndoHistory::Get().Push(
            std::move(label),
            [action, &keyframeManager] {
                action->GetUndoAction()->DoAction();
                keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(action->property));
            },
            [action, &keyframeManager] {
                action->DoAction();
                keyframeManager.SortAndSaveKeyframes(keyframeManager.GetKeyframes(action->property));
            });
    }

    void KeyframeManager::ModifyTickAction::DoAction() const
    {
        if (auto it = GetKeyframe(id); it != GetKeyframes().end())
            it->tick = newTick;
    }

    std::unique_ptr<KeyframeManager::KeyframeAction> KeyframeManager::ModifyTickAction::GetUndoAction() const
    {
        return std::make_unique<ModifyTickAction>(property,newTick,oldTick,id);
    }

    std::vector<Types::Keyframe>::iterator KeyframeManager::KeyframeAction::GetKeyframe(uint32_t id) const
    {
        auto& keyframes = GetKeyframes();
        auto it = std::find_if(keyframes.begin(), keyframes.end(), [&](const Types::Keyframe& kf) { return kf.id == id; });
        return it;
    }

    std::vector<Types::Keyframe>& KeyframeManager::KeyframeAction::GetKeyframes() const
    {
        return KeyframeManager::Get().GetKeyframes(property);
    }

    void KeyframeManager::ModifyValueAction::DoAction() const
    {
        if (auto it = GetKeyframe(id); it != GetKeyframes().end())
            it->value = newValue;
    }

    std::unique_ptr<KeyframeManager::KeyframeAction> KeyframeManager::ModifyValueAction::GetUndoAction() const
    {
        return std::make_unique<ModifyValueAction>(property, newValue, oldValue, id);
    }

    void KeyframeManager::ModifyTickAndValueAction::DoAction() const
    {
        if (auto it = GetKeyframe(id); it != GetKeyframes().end())
        {
            it->tick = newTick;
            it->value = newValue;
        }
    }

    std::unique_ptr<KeyframeManager::KeyframeAction> KeyframeManager::ModifyTickAndValueAction::GetUndoAction() const
    {
        return std::make_unique<ModifyTickAndValueAction>(property,newTick,oldTick,newValue,oldValue,id);
    }

    void KeyframeManager::RemoveKeyframesAction::DoAction() const
    {
        for (auto& keyframe : keyframes)
        {
            if (auto it = GetKeyframe(keyframe.id); it != GetKeyframes().end())
            {
                GetKeyframes().erase(it);
            }
        }
    }

    std::unique_ptr<KeyframeManager::KeyframeAction> KeyframeManager::RemoveKeyframesAction::GetUndoAction() const
    {
        return std::make_unique<AddKeyframesAction>(property,keyframes);
    }

    void KeyframeManager::AddKeyframesAction::DoAction() const
    {
        for (auto& keyframe : keyframes)
        {
            GetKeyframes().emplace_back(keyframe);
        }
    }

    std::unique_ptr<KeyframeManager::KeyframeAction> KeyframeManager::AddKeyframesAction::GetUndoAction() const
    {
        return std::make_unique<RemoveKeyframesAction>(property, keyframes);
    }

    void KeyframeManager::ChangeKeyframesAction::DoAction() const
    {
        for (const auto& key : after)
        {
            if (auto it = GetKeyframe(key.id); it != GetKeyframes().end())
                *it = key;
        }
    }

    std::unique_ptr<KeyframeManager::KeyframeAction> KeyframeManager::ChangeKeyframesAction::GetUndoAction() const
    {
        return std::make_unique<ChangeKeyframesAction>(property, after, before);
    }

}  // namespace IWXMVM::Components
