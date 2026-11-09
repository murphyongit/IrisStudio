#include "StdInclude.hpp"
#include "ActorActions.hpp"

#include "Mod.hpp"
#include "Components/Actors.hpp"
#include "Components/CameraManager.hpp"
#include "Components/DropLogic.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/LightLogic.hpp"
#include "Components/Lights.hpp"
#include "Components/Playback.hpp"
#include "Components/PropLogic.hpp"
#include "Components/Props.hpp"
#include "Graphics/Graphics.hpp"
#include "UI/Commands.hpp"
#include "UI/ImGuiEx/KeyframeableControls.hpp"
#include "UI/Toasts.hpp"
#include "UI/UIManager.hpp"

namespace IWXMVM::UI::ActorActions
{
    using ActorLogic::Start;

    const char* GetStartLabel(Start start)
    {
        switch (start)
        {
            case Start::Standing:
                return "Standing";
            case Start::WalkPath:
                return "Walking a path";
            case Start::RunPath:
                return "Running a path";
            case Start::Shoot:
                return "Shooting from here";
            case Start::Die:
                return "Dying on cue";
        }
        return "";
    }

    const char* GetStartTooltip(Start start)
    {
        switch (start)
        {
            case Start::Standing:
                return "Stands idling from the playhead on";
            case Start::WalkPath:
                return "Walks from the playhead along the points you click on the ground, then stands; Enter when "
                       "done";
            case Start::RunPath:
                return "Runs from the playhead along the points you click on the ground, then stands; Enter when "
                       "done";
            case Start::Shoot:
                return "Holds a rifle and fires a burst at the playhead";
            case Start::Die:
                return "Stands, then is hit and dies a second after the playhead; drag the hit on the timeline to "
                       "time it";
        }
        return "";
    }

    const char* GetStartName(Start start)
    {
        switch (start)
        {
            case Start::Standing:
                return "standing";
            case Start::WalkPath:
                return "walk";
            case Start::RunPath:
                return "run";
            case Start::Shoot:
                return "shoot";
            case Start::Die:
                return "die";
        }
        return "";
    }

    namespace
    {
        // the dog the dog starting points add
        constexpr std::string_view DOG = Components::CharacterLogic::MAP_DOGS[0];

        ActorLogic::Body GetStartBody(Types::ActorKind kind)
        {
            return kind == Types::ActorKind::Dog ? ActorLogic::Body::Dog(ActorLogic::DogBreed::Shepherd)
                                                 : ActorLogic::Body{};
        }
    }  // namespace

    std::optional<std::string> GetStartUnavailableReason(Start start, Types::ActorKind kind)
    {
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog)
            return "Finding the characters of this map...";
        if (kind == Types::ActorKind::Dog)
        {
            if (!catalog->FindModel(DOG))
                return "This map has no dog";
            if (start == Start::Shoot)
                return "A dog holds no weapon";
        }
        const auto animations = Components::Actors::GetStartAnimations(GetStartBody(kind));
        switch (start)
        {
            case Start::WalkPath:
                if (animations.walk.empty())
                    return "This map has no walk for actors";
                break;
            case Start::RunPath:
                if (animations.run.empty())
                    return "This map has no run for actors";
                break;
            case Start::Shoot:
                if (Components::Actors::GetStartWeapon().empty())
                    return "Finding the weapons of this map...";
                break;
            case Start::Die:
                if (animations.death.empty())
                    return "This map has no death for actors";
                break;
            default:
                break;
        }
        return std::nullopt;
    }

    namespace
    {
        // the map's own multiplayer character (the first of "This map": never a character zone's or the campaign's),
        // else a character of the pack if there is one, else a multiplayer body with a head
        std::vector<std::string> GetDefaultModels(const Types::ActorCatalog& catalog, std::string* character = nullptr)
        {
            for (const auto& each : catalog.characters)
                if (each.level.empty() && each.kind == Types::Character::Kind::Human &&
                    catalog.HasModels(each.GetModels()))
                {
                    if (character)
                        *character = each.id;
                    return each.GetModels();
                }
            using ModelKind = Types::ActorCatalog::ModelKind;
            auto first = [&](ModelKind kind, bool fromPack) -> std::string {
                for (const auto& model : catalog.models)
                    if (model.kind == kind && (model.source != "Multiplayer") == fromPack)
                        return model.name;
                return {};
            };
            std::vector<std::string> models;
            if (auto body = first(ModelKind::FullBody, true); !body.empty())
                models = {body};
            else if (body = first(ModelKind::Body, true); !body.empty())
                models = {body, first(ModelKind::Head, true)};
            else if (body = first(ModelKind::Body, false); !body.empty())
                models = {body, first(ModelKind::Head, false)};
            std::erase(models, std::string());
            return models;
        }

        std::uint32_t GetTick()
        {
            return Components::Playback::GetActorTick();
        }

        // the actor's step of the undo history, named, with the clip or event it is about selected (which the side
        // panel then shows)
        void Change(const Types::Actor& actor, std::string label)
        {
            Components::Actors::Update(actor, false, label);
            Components::Actors::EndEditStep();
        }

        // how to stop placing points, for the messages: "Enter"
        std::string GetFinishKey()
        {
            const auto key = Commands::GetShortcut("placing.finish");
            return key.empty() ? "Enter" : key;
        }

        // places the points of the actor's path where the view is clicked next, its walk as long as they take
        void PlacePoints(const Types::Actor& actor, std::uint32_t walkClip, float guessedSpeed)
        {
            Components::Actors::Select(actor.id);
            Components::Actors::SelectWaypoint(std::nullopt);
            Components::Actors::FitWalkWhilePlacing(actor.id, walkClip, guessedSpeed);
            GFX::SetPlacingPathPoints(true);
            UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);
        }
    }  // namespace

    void Add(Start start, glm::vec3 origin, glm::vec3 direction, Types::ActorKind kind)
    {
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog || GetStartUnavailableReason(start, kind))
            return;
        const auto tick = GetTick();
        const bool dog = kind == Types::ActorKind::Dog;
        const auto animations = Components::Actors::GetStartAnimations(GetStartBody(kind));
        auto actor = ActorLogic::MakeStart(start, animations, tick, Mod::GetGameInterface()->GetDemoInfo().endTick,
                                           dog ? std::string() : Components::Actors::GetStartWeapon());
        if (dog)
        {
            actor.models = {std::string(DOG)};
            actor.character = "map/" + std::string(DOG);
            actor.name = Components::Actors::MakeName("Dog");
        }
        else
            actor.models = GetDefaultModels(*catalog, &actor.character);
        actor.id = Components::Actors::Add(actor);
        actor = *Components::Actors::GetActor(actor.id);

        // facing the camera
        const auto yaw =
            std::remainder(Components::CameraManager::Get().GetActiveCamera()->GetRotation().y + 180.0f, 360.0f);
        Components::Actors::RequestPlacement(actor.id, origin, direction, std::nullopt, yaw);
        Components::Actors::Select(actor.id);
        GFX::GraphicsManager::Get().FocusActor();
        UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);

        if (start == Start::WalkPath || start == Start::RunPath)
        {
            const auto walk = std::ranges::find(
                actor.clips, start == Start::WalkPath ? animations.walk : animations.run, &Types::ActorClip::animation);
            if (walk == actor.clips.end())
                return;
            PlacePoints(actor, walk->id,
                        actor.locomotion == Types::ActorLocomotion::Auto ? actor.pathSpeed
                        : start == Start::WalkPath                       ? ActorLogic::WALK_SPEED_GUESS
                                                                         : ActorLogic::RUN_SPEED_GUESS);
            Toasts::Show(std::format("{} added: click the ground where it {}s, {} when done", actor.name,
                                     start == Start::WalkPath ? "walk" : "run", GetFinishKey()));
        }
    }

    void AddActor(glm::vec3 origin, glm::vec3 direction, Types::ActorKind kind)
    {
        Add(Start::Standing, origin, direction, kind);
        // a person holds an assault rifle of the map's, as its idle does
        if (kind != Types::ActorKind::Human)
            return;
        const auto selected = Components::Actors::GetSelected();
        auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
        if (actor && actor->weapon.empty())
        {
            actor->weapon = Components::Actors::GetStartWeapon();
            Components::Actors::Update(*actor, true);
        }
    }

    std::optional<std::string> GetAddUnavailableReason(Types::ActorKind kind)
    {
        return GetStartUnavailableReason(Start::Standing, kind);
    }

    std::optional<std::string> GetKillstreakUnavailableReason(Types::Killstreak killstreak)
    {
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog)
            return "Finding the characters of this map...";
        const auto& info = Components::KillstreakLogic::Get(killstreak);
        if (!catalog->FindModel(info.models.front()))
            return std::format("This map has no {}", info.name);
        return std::nullopt;
    }

    std::string GetKillstreakTooltip(Types::Killstreak killstreak)
    {
        const auto& info = Components::KillstreakLogic::Get(killstreak);
        switch (info.vehicleClass)
        {
            case Types::VehicleClass::Car:
                return "Drives from the playhead along the points you click on the ground, then stops; Enter when done";
            case Types::VehicleClass::Helicopter:
                return std::format("Flies from the playhead {:.0f} units above the points you click, then hovers; Enter "
                                   "when done",
                                   info.flyHeight);
            case Types::VehicleClass::Plane:
                return std::format("Flies over where you clicked at the playhead, {:.0f} units up, toward the camera, as "
                                   "the game's pass",
                                   info.flyHeight);
            case Types::VehicleClass::Turret:
                return killstreak == Types::Killstreak::SentryGun ? "Stands, scanning from side to side"
                                                                  : "Stands, facing the camera";
            case Types::VehicleClass::Crate:
                return "Stands where you clicked";
        }
        return {};
    }

    std::string GetKillstreakKey(Types::Killstreak killstreak)
    {
        std::string key;
        for (const auto c : Components::KillstreakLogic::Get(killstreak).name)
            if (std::isalnum(static_cast<unsigned char>(c)))
                key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return key;
    }

    std::optional<std::string> GetEffectUnavailableReason(std::string_view effect)
    {
        if (!Components::Actors::GetEffectInfo(effect))
            return std::format("This map hasn't {}", effect);
        return std::nullopt;
    }

    void AddEffect(const std::string& effect, glm::vec3 origin, glm::vec3 direction)
    {
        const auto info = Components::Actors::GetEffectInfo(effect);
        if (!info)
            return;
        // at the camera until the game finds the surface (see Components::Actors::Place)
        auto actor = ActorLogic::MakeEffect(effect, *info, origin, {0.0f, 0.0f, 1.0f}, GetTick());
        actor.name = Components::Actors::MakeName(ActorLogic::GetEffectBaseName(effect));
        actor.id = Components::Actors::Add(actor);
        Components::Actors::RequestPlacement(actor.id, origin, direction);
        Components::Actors::AddRecentEffect(effect);
        Components::Actors::Select(actor.id);
        UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);
    }

    void AddProp(const std::string& model, glm::vec3 origin, glm::vec3 direction)
    {
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog || !std::ranges::binary_search(catalog->props, model))
            return;
        const auto yaw = GetYawFacingCamera();
        // at the camera until the game finds the surface (see Components::Actors::Place)
        auto actor =
            ActorLogic::MakeProp(model, origin, yaw, GetTick(), Mod::GetGameInterface()->GetDemoInfo().endTick);
        actor.name = Components::Actors::MakeName(ActorLogic::GetPropBaseName(model));
        actor.id = Components::Actors::Add(actor);
        Components::Actors::RequestPlacement(actor.id, origin, direction, std::nullopt, yaw);
        Components::Actors::AddRecentProp(model);
        Components::Actors::Select(actor.id);
        UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);
    }

    float GetYawFacingCamera()
    {
        return std::remainder(Components::CameraManager::Get().GetActiveCamera()->GetRotation().y + 180.0f, 360.0f);
    }

    void KeyPropPlace(std::uint32_t id, std::optional<glm::vec3> position, std::optional<glm::vec3> turn,
                      const std::string& step)
    {
        using Channel = Components::PropLogic::Channel;
        const auto actor = Components::Actors::GetActor(id);
        if (!actor || !actor->placedProp)
            return;
        if (position)
            ImGuiEx::Keyframeable::KeyValueHere(
                Components::Props::GetProperty(actor->placedProp->id, Channel::Position),
                Types::KeyframeValue(*position), step.c_str(), step);
        if (turn)
        {
            const auto& property = Components::Props::GetProperty(actor->placedProp->id, Channel::Turn);
            const auto value =
                Components::PropLogic::GetTurnForKey(Components::KeyframeManager::Get().GetKeyframes(property),
                                                     Components::Playback::GetTimelineTick(), *turn);
            ImGuiEx::Keyframeable::KeyValueHere(property, Types::KeyframeValue(value), step.c_str(), step);
        }
    }

    void MoveProp(Types::Actor& actor, const Types::Actor& placed, const std::string& step)
    {
        using Channel = Components::PropLogic::Channel;
        if (!actor.placedProp || !placed.placedProp)
            return;
        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto from = Components::Props::WithKeyedPlacementNow(actor, Components::Playback::GetTimelineTick());
        std::optional<glm::vec3> position, turn;
        for (const auto channel : Components::PropLogic::ALL_CHANNELS)
        {
            auto value = Components::PropLogic::GetValue(placed, channel);
            if (value.vector3 == Components::PropLogic::GetValue(from, channel).vector3)
                continue;
            if (keyframeManager.GetKeyframes(Components::Props::GetProperty(actor.placedProp->id, channel)).empty())
            {
                if (channel == Channel::Turn)
                    value.vector3.y = std::remainder(value.vector3.y, 360.0f);
                Components::PropLogic::SetValue(actor, channel, value);
            }
            else
                (channel == Channel::Position ? position : turn) = value.vector3;
        }
        if (position || turn)
            KeyPropPlace(actor.id, position, turn, step);
    }

    void AddLight(Types::LightKind kind, glm::vec3 origin, glm::vec3 direction)
    {
        // at the camera until the game finds the surface (see Components::Actors::Place)
        auto actor = Components::LightLogic::MakeLight(kind, origin, {0.0f, 0.0f, 1.0f}, origin, GetTick());
        actor.name = Components::Actors::MakeName(Components::LightLogic::GetBaseName(kind));
        actor.id = Components::Actors::Add(actor);
        Components::Actors::RequestPlacement(actor.id, origin, direction);
        Components::Actors::Select(actor.id);
        UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);
    }

    void KeyLightPlace(std::uint32_t id, std::optional<glm::vec3> position, std::optional<glm::vec3> aim,
                       const std::string& step)
    {
        using Channel = Components::LightLogic::Channel;
        const auto actor = Components::Actors::GetActor(id);
        if (!actor || !actor->light)
            return;
        if (position)
            ImGuiEx::Keyframeable::KeyValueHere(Components::Lights::GetProperty(actor->light->id, Channel::Position),
                                                Types::KeyframeValue(*position), step.c_str(), step);
        if (aim)
        {
            const auto& property = Components::Lights::GetProperty(actor->light->id, Channel::Aim);
            const auto value = Components::LightLogic::GetAimForKey(
                Components::KeyframeManager::Get().GetKeyframes(property), Components::Playback::GetTimelineTick(),
                glm::vec3(std::clamp(aim->x, -90.0f, 90.0f), aim->y, 0.0f));
            ImGuiEx::Keyframeable::KeyValueHere(property, Types::KeyframeValue(value), step.c_str(), step);
        }
    }

    void MoveLight(Types::Actor& actor, const Types::Actor& placed, const std::string& step)
    {
        using Channel = Components::LightLogic::Channel;
        if (!actor.light || !placed.light)
            return;
        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto from = Components::Lights::WithKeyedPlacementNow(actor, Components::Playback::GetTimelineTick());
        std::optional<glm::vec3> position, aim;
        for (const auto channel : {Channel::Position, Channel::Aim})
        {
            auto value = Components::LightLogic::GetValue(placed, channel);
            if (value.vector3 == Components::LightLogic::GetValue(from, channel).vector3)
                continue;
            if (keyframeManager.GetKeyframes(Components::Lights::GetProperty(actor.light->id, channel)).empty())
            {
                if (channel == Channel::Aim)
                    value.vector3.y = std::remainder(value.vector3.y, 360.0f);
                Components::LightLogic::SetValue(actor, channel, value);
            }
            else
                (channel == Channel::Position ? position : aim) = value.vector3;
        }
        if (position || aim)
            KeyLightPlace(actor.id, position, aim, step);
    }

    void AddKillstreak(Types::Killstreak killstreak, glm::vec3 origin, glm::vec3 direction)
    {
        if (GetKillstreakUnavailableReason(killstreak))
            return;
        const auto& info = Components::KillstreakLogic::Get(killstreak);
        const auto tick = GetTick();
        // facing the camera
        const auto yaw =
            std::remainder(Components::CameraManager::Get().GetActiveCamera()->GetRotation().y + 180.0f, 360.0f);
        auto actor = Components::KillstreakLogic::MakeStart(killstreak, tick,
                                                             Mod::GetGameInterface()->GetDemoInfo().endTick, yaw);
        actor.name = Components::Actors::MakeName(std::string(info.name));
        actor.id = Components::Actors::Add(actor);
        actor = *Components::Actors::GetActor(actor.id);

        Components::Actors::RequestPlacement(actor.id, origin, direction, std::nullopt, yaw,
                                             Components::KillstreakLogic::GetStartPlaceTick(actor, tick));
        Components::Actors::Select(actor.id);
        GFX::GraphicsManager::Get().FocusActor();
        UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);
    }

    void Play(std::uint32_t id, const std::string& animation, std::optional<std::uint32_t> tick)
    {
        auto actor = Components::Actors::GetActor(id);
        const auto catalog = Components::Actors::GetCatalog();
        const auto found = catalog ? catalog->FindAnimation(animation) : nullptr;
        if (!actor || !found)
            return;
        if (found->kind == Types::AnimationKind::Face)
        {
            PlayFace(id, animation, tick);
            return;
        }
        if (const auto misfit = Components::Actors::DescribeMisfit(actor->models, animation); !misfit.empty())
            Toasts::Show(std::format("{} doesn't fit {}: {}", animation, actor->name, misfit));
        // a moving loop played where the actor walks its path: the look of that stretch, which the path walks as fast
        // as it goes, not a clip that would walk it at its own speed
        const auto at = tick.value_or(GetTick());
        if (const auto* entry = found->entry; entry && found->loop && actor->locomotion == Types::ActorLocomotion::Auto)
            if (const auto stretch = Components::Actors::GetStretchAt(*actor, at))
            {
                const auto timing = Components::Actors::GetPathTiming(*actor);
                const auto speed = glm::length(glm::vec2(entry->motion)) / std::max(entry->Duration(), 0.01f);
                const auto angle = glm::degrees(std::atan2(entry->motion.y, entry->motion.x));
                if (*stretch + 1 < timing.reached.size() && timing.reached[*stretch + 1] &&
                    at < *timing.reached[*stretch + 1] && speed >= ActorLogic::IDLE_SPEED &&
                    std::abs(angle) <= TransitionLogic::MAX_WALK_ANGLE)
                {
                    Components::Actors::SetLook(*actor, stretch, animation);
                    Components::Actors::Select(id);
                    Change(*actor, std::format("look {} for a stretch of {}", animation, actor->name));
                    Components::Actors::AddTriedLook(id, animation);
                    Components::Actors::AddRecentAnimation(animation);
                    return;
                }
            }
        // one play, blending in as long as joins it cleanly to what shows there
        const auto clip = ActorLogic::PlayAt(*actor, at, animation, found->duration, found->loop, found->notetracks);
        Components::Actors::JoinAround(*actor, clip);
        Components::Actors::Select(id);
        Change(*actor, std::format("play {} on {}", animation, actor->name));
        Components::Actors::BlendCleanly(id, clip);
        Components::Actors::SelectClip(clip);
        Components::Actors::AddRecentAnimation(animation);
    }

    void KeepDeathAt(std::uint32_t id, std::uint32_t death, std::uint32_t start)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor)
            return;
        const auto found = std::ranges::find(actor->clips, death, &Types::ActorClip::id);
        if (found == actor->clips.end() || found->start == start)
            return;
        found->start = start;
        // (what runs into it ends there, what would start after it goes: it lies there for good)
        std::erase_if(actor->clips,
                      [&](const Types::ActorClip& clip) { return clip.id != death && clip.start >= start; });
        for (auto& clip : actor->clips)
            if (clip.id != death && clip.End() > start)
                clip.length = std::max(start - clip.start, 1u);
        Components::Actors::Update(*actor, true);
    }

    void PlayAfter(std::uint32_t id, std::uint32_t clip, const std::string& animation)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor)
            return;
        if (const auto misfit = Components::Actors::DescribeMisfit(actor->models, animation); !misfit.empty())
            Toasts::Show(std::format("{} doesn't fit {}: {}", animation, actor->name, misfit));
        const auto added = Components::Actors::PlayAfter(*actor, clip, animation);
        if (added == 0)
            return;
        // (a death after the walk dies as it runs, on the path's end)
        if (const auto found = std::ranges::find(actor->clips, added, &Types::ActorClip::id);
            found != actor->clips.end() && ActorLogic::IsDeathClip(*found, Components::Actors::GetAnimationLookup()))
            Components::Actors::AnchorDeath(*actor, added);
        Components::Actors::Select(id);
        Change(*actor, std::format("then {} on {}", animation, actor->name));
        Components::Actors::BlendCleanly(id, added);
        Components::Actors::SelectClip(added);
        Components::Actors::AddRecentAnimation(animation);
    }

    std::optional<std::uint32_t> GetClipAtPlayhead(const Types::Actor& actor)
    {
        const auto tick = GetTick();
        const auto shown = std::ranges::find_if(actor.clips.rbegin(), actor.clips.rend(),
                                                [&](const Types::ActorClip& clip) { return clip.start <= tick; });
        return shown != actor.clips.rend() ? std::optional(shown->id) : std::nullopt;
    }

    void PlayFace(std::uint32_t id, const std::string& animation, std::optional<std::uint32_t> tick)
    {
        auto actor = Components::Actors::GetActor(id);
        const auto catalog = Components::Actors::GetCatalog();
        const auto found = catalog ? catalog->FindAnimation(animation) : nullptr;
        if (!actor || !found)
            return;
        // (the face's animations are a person's)
        if (const auto kind = Components::Actors::GetActorKind(*actor); kind != Types::ActorKind::Human)
        {
            Toasts::Show(std::format("{} is a {}: {} is a person's face", actor->name,
                                     kind == Types::ActorKind::Dog ? "dog" : "vehicle", animation));
            return;
        }
        // once, or looping up to the next face clip (for 5 s with none)
        const auto at = tick.value_or(GetTick());
        Types::ActorClip clip{
            .animation = animation,
            .start = at,
            .length = std::max(1u, static_cast<std::uint32_t>(std::lround(found->duration * 1000.0f))),
            .loop = found->loop,
            .blendIn = ActorLogic::FACE_BLEND};
        if (found->loop)
        {
            clip.length = 5000;
            for (const auto& other : actor->faceClips)
                if (other.start > at)
                    clip.length = std::min(clip.length, other.start - at);
        }
        const auto added = ActorLogic::AddFaceClip(*actor, clip);
        Components::Actors::Select(id);
        Change(*actor, std::format("play {} on the face of {}", animation, actor->name));
        Components::Actors::SelectFaceClip(added);
        Components::Actors::AddRecentAnimation(animation);
    }

    std::optional<std::string> GetWalkUnavailableReason(std::uint32_t id, bool clickedPoints)
    {
        const auto actor = Components::Actors::GetActor(id);
        if (!actor)
            return "No such actor";
        if (actor->movement == Types::ActorMovement::Attached)
            return "It moves with what it is attached to";
        if (clickedPoints && actor->movement == Types::ActorMovement::Keyframes)
            return "It moves by keys: right-click the ground to walk it there";
        if (const auto body = Components::Actors::GetBody(*actor); body.kind == Types::ActorKind::Vehicle)
        {
            const auto& info = Components::KillstreakLogic::Get(body.killstreak);
            if (info.vehicleClass == Types::VehicleClass::Plane)
                return "A plane flies its pass: move its points, or add more";
            if (info.vehicleClass != Types::VehicleClass::Car && info.vehicleClass != Types::VehicleClass::Helicopter)
                return std::format("The {} stays where it is", info.name);
            return std::nullopt;
        }
        if (const auto animations = Components::Actors::GetStartAnimations(Components::Actors::GetBody(*actor));
            animations.walk.empty() && animations.run.empty())
            return "This map has no walk for actors";
        return std::nullopt;
    }

    void WalkTo(std::uint32_t id, glm::vec3 origin, glm::vec3 direction, bool run, std::optional<Types::ActorGait> gait,
                bool crouched)
    {
        const auto actor = Components::Actors::GetActor(id);
        if (!actor || (run && !gait ? GetRunUnavailableReason(id) : GetWalkUnavailableReason(id, false)))
            return;
        Components::Actors::Select(id);
        GFX::GraphicsManager::Get().FocusActor();
        Components::Actors::RequestWalkTo(id, origin, direction, run, std::nullopt, gait, crouched);
        const auto body = Components::Actors::GetBody(*actor);
        const auto goes = body.kind != Types::ActorKind::Vehicle ? (gait == Types::ActorGait::Sprint       ? "sprints"
                                                                    : run || gait == Types::ActorGait::Run ? "runs"
                                                                                                           : "walks")
                          : Components::KillstreakLogic::Get(body.killstreak).vehicleClass == Types::VehicleClass::Car
                              ? "drives"
                              : "flies";
        Toasts::Show(std::format("{} {} there from {}", actor->name, goes, Toasts::FormatTime(GetTick())));
    }

    std::optional<std::string> GetRunUnavailableReason(std::uint32_t id)
    {
        if (auto reason = GetWalkUnavailableReason(id, false))
            return reason;
        const auto actor = Components::Actors::GetActor(id);
        if (actor && actor->locomotion != Types::ActorLocomotion::Auto)
            return "It runs with Locomotion Auto (Movement page)";
        return std::nullopt;
    }

    void WalkOn(std::uint32_t id, std::optional<Types::ActorGait> gait, bool crouched)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor || GetWalkUnavailableReason(id, true))
            return;
        // from where it stands, to points to come
        const auto tick = GetTick();
        float guessedSpeed = 0.0f;
        const auto here = Components::Actors::EvaluatePlacement(*actor, tick).position;
        const auto clip =
            Components::Actors::WalkTo(*actor, tick, here, &guessedSpeed, {.gait = gait, .crouched = crouched});
        if (clip == 0)
            return;
        Change(*actor, std::format("walk {} on", actor->name));
        PlacePoints(*actor, clip, guessedSpeed);
    }

    void LookAtCamera(std::uint32_t id)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor)
            return;
        const auto tick = GetTick();
        // the Look there, or a new one
        std::uint32_t event = 0;
        const auto look = std::ranges::find_if(actor->events, [&](const Types::ActorEvent& each) {
            return each.type == Types::ActorEventType::Look && each.tick == tick;
        });
        if (look != actor->events.end())
        {
            look->lookTarget = Types::ActorLookTarget::Camera;
            event = look->id;
        }
        else
        {
            event = ActorLogic::AddEvent(
                *actor,
                {.type = Types::ActorEventType::Look, .tick = tick, .lookTarget = Types::ActorLookTarget::Camera});
        }
        Components::Actors::Select(id);
        Change(*actor, std::format("{} looks at the camera", actor->name));
        Components::Actors::SelectEvent(event);
    }

    void AddMortar(glm::vec3 origin, glm::vec3 direction)
    {
        if (GetMortarUnavailableReason())
            return;
        // (its shells fly away from the camera, onto what it looks at)
        const auto yaw = std::remainder(GetYawFacingCamera() + 180.0f, 360.0f);
        auto actor = Components::KillstreakLogic::MakeMortar(origin, yaw, GetTick());
        actor.name = Components::Actors::MakeName(std::string(Components::KillstreakLogic::MORTAR_NAME));
        actor.id = Components::Actors::Add(actor);
        Components::Actors::RequestPlacement(actor.id, origin, direction);
        Components::Actors::Select(actor.id);
        UIManager::Get().ShowPicked(PanelLogic::Kind::Actor);
    }

    std::optional<std::string> GetMortarUnavailableReason()
    {
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog || !std::ranges::binary_search(catalog->props, Components::DropLogic::MORTAR_MODEL))
            return "This map hasn't its shells";
        return std::nullopt;
    }

    void AddKillstreakFire(std::uint32_t id)
    {
        namespace Killstreaks = Components::KillstreakLogic;
        auto actor = Components::Actors::GetActor(id);
        // (the Mortar Team's: its shells, the first landing at the playhead)
        if (actor && actor->mortar)
        {
            const auto event = ActorLogic::AddEvent(*actor, Killstreaks::MakeMortarFire(GetTick()));
            Components::Actors::Select(id);
            Change(*actor, std::format("{} fires", actor->name));
            Components::Actors::SelectEvent(event);
            return;
        }
        const auto killstreak = actor ? Killstreaks::FindByModels(actor->models) : std::nullopt;
        if (!killstreak)
            return;
        const auto& info = Killstreaks::Get(*killstreak);
        if (!Killstreaks::CanFire(info, Killstreaks::GetLook(info, actor->models.front())))
            return;
        if (info.guns.empty() && info.fireDrop == Killstreaks::FireDrop::Detonate)
        {
            DieHere(id, std::string(Killstreaks::MOTION_DETONATE));
            return;
        }
        const auto event = ActorLogic::AddEvent(*actor, Killstreaks::MakeFire(info, *actor, GetTick()));
        Components::Actors::Select(id);
        Change(*actor, std::format("{} fires", actor->name));
        Components::Actors::SelectEvent(event);
    }

    std::optional<std::string> GetFireUnavailableReason(const Types::Actor& actor)
    {
        namespace Killstreaks = Components::KillstreakLogic;
        const auto kind = Components::Actors::GetActorKind(actor);
        if (kind == Types::ActorKind::Mortar)
            return std::nullopt;
        if (kind == Types::ActorKind::Dog)
            return "A dog holds no weapon";
        if (kind == Types::ActorKind::Vehicle)
        {
            const auto killstreak = Killstreaks::FindByModels(actor.models);
            if (!killstreak)
                return "The vehicle has no gun";
            const auto& info = Killstreaks::Get(*killstreak);
            if (Killstreaks::CanFire(info, Killstreaks::GetLook(info, actor.models.front())))
                return std::nullopt;
            return std::format("The {} has no gun", info.name);
        }
        if (actor.weapon.empty())
            return "Give it a weapon first";
        return std::nullopt;
    }

    void DieHere(std::uint32_t id, const std::string& animation)
    {
        auto actor = Components::Actors::GetActor(id);
        const auto catalog = Components::Actors::GetCatalog();
        const auto death = !animation.empty() ? animation
                           : actor            ? Components::Actors::GetDeathAnimation(&*actor)
                                              : std::string();
        const auto found = catalog ? catalog->FindAnimation(death) : nullptr;
        if (!actor || !found)
            return;
        const auto hit = ActorLogic::AddDeath(*actor, GetTick(), death, found->duration);
        Components::Actors::Select(id);
        Change(*actor, std::format("kill {}", actor->name));
        // (blending in as long as joins it cleanly)
        if (const auto event = std::ranges::find(actor->events, hit, &Types::ActorEvent::id);
            event != actor->events.end())
            if (const auto* clip = ActorLogic::FindDeathClip(*actor, *event))
                Components::Actors::BlendCleanly(id, clip->id);
        Components::Actors::SelectEvent(hit);
    }

    std::vector<std::string> GetDeaths(const Types::Actor& actor)
    {
        std::vector<std::string> deaths;
        const auto catalog = Components::Actors::GetCatalog();
        if (!catalog)
            return deaths;
        if (auto first = Components::Actors::GetDeathAnimation(&actor); !first.empty())
            deaths.push_back(std::move(first));
        for (const auto& list : {Components::Actors::GetRecentAnimations()})
            for (const auto& name : list)
            {
                const auto animation = catalog->FindAnimation(name);
                if (animation && Components::Actors::GetAnimationCategory(*animation) == "Deaths" &&
                    Components::Actors::Fits(actor.models, name).value_or(true) &&
                    std::ranges::find(deaths, name) == deaths.end())
                    deaths.push_back(name);
            }
        return deaths;
    }

    void AddPathPoint(std::uint32_t id, glm::vec3 origin, glm::vec3 direction,
                      std::optional<std::pair<Types::ActorGait, bool>> gait)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor || actor->movement != Types::ActorMovement::Path)
            return;
        const auto selected =
            Components::Actors::GetSelected() == id ? Components::Actors::GetSelectedWaypoint() : std::nullopt;
        const auto index = ActorLogic::InsertWaypoint(
            *actor, selected.value_or(std::max<std::size_t>(actor->path.size(), 1) - 1), gait);
        // (its animation as the body has that gait)
        if (gait && index > 0 && !actor->stretches.empty())
            Components::Actors::SetGait(*actor, index - 1, index, gait->first, gait->second);
        Components::Actors::Select(id);
        Components::Actors::Update(*actor, false, std::format("add path point to {}", actor->name));
        Components::Actors::SelectWaypoint(index);
        // the game puts it on the ground, part of the same step
        Components::Actors::RequestPlacement(id, origin, direction, index);
    }

    void InsertPathPoint(std::uint32_t id, std::size_t after, glm::vec3 origin, glm::vec3 direction)
    {
        Components::Actors::SelectWaypoint(after);
        AddPathPoint(id, origin, direction);
    }

    void DeletePathPoint(std::uint32_t id, std::size_t point)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor || point >= actor->path.size() || actor->path.size() <= 1)
            return;
        ActorLogic::RemoveWaypoint(*actor, point);
        Change(*actor, std::format("delete path point of {}", actor->name));
        Components::Actors::SelectWaypoint(std::nullopt);
        Toasts::Show(std::format("Point {} of {} deleted", point + 1, actor->name), true);
    }

    void EndPathAt(std::uint32_t id, std::size_t point)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor || point + 1 >= actor->path.size())
            return;
        actor->path.resize(std::max<std::size_t>(point + 1, 1));
        ActorLogic::FitStretches(*actor);
        Change(*actor, std::format("end the path of {}", actor->name));
        Components::Actors::SelectWaypoint(point);
    }

    void SetWait(std::uint32_t id, std::size_t point, std::uint32_t wait)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor || point >= actor->path.size())
            return;
        if (!Components::Actors::MakeStretches(*actor))
            return;
        ActorLogic::FitStretches(*actor);
        actor->stretches[std::min(point, actor->stretches.size() - 1)].wait = wait;
        Change(*actor, std::format("wait at a point of {}", actor->name));
    }

    void MovePointToCrosshair(std::uint32_t id, std::size_t point)
    {
        const auto actor = Components::Actors::GetActor(id);
        if (!actor || point >= actor->path.size())
            return;
        const auto& camera = Components::CameraManager::Get().GetActiveCamera();
        Components::Actors::RequestPlacement(id, camera->GetPosition(), camera->GetForwardVector(), point);
    }

    void SetGait(std::uint32_t id, std::size_t from, std::size_t to, Types::ActorGait gait, bool crouched)
    {
        auto actor = Components::Actors::GetActor(id);
        if (!actor)
            return;
        Components::Actors::SetGait(*actor, from, to, gait, crouched);
        Change(*actor, std::format("change the gait of {}", actor->name));
    }

    void MoveHere(std::uint32_t id, glm::vec3 origin, glm::vec3 direction)
    {
        if (!Components::Actors::GetActor(id))
            return;
        Components::Actors::RequestPlacement(id, origin, direction);
    }

    std::string GetGaitName(const Types::Actor& actor, Types::ActorGait gait, bool crouched)
    {
        for (const auto& preset : Components::Actors::GetGaits(actor))
            if (preset.gait == ActorLogic::ToGait(gait) && (preset.stance == Types::ActorStance::Crouch) == crouched)
                return std::string(preset.name);
        constexpr std::array NAMES{"Walk", "Run", "Sprint"};
        return std::format("{}{}", crouched ? "Crouch " : "", NAMES[static_cast<std::size_t>(gait)]);
    }

    void Duplicate(std::uint32_t id)
    {
        if (const auto copy = Components::Actors::Duplicate(id))
            Components::Actors::Select(copy);
    }

    void DuplicateBeside(std::uint32_t id, bool left)
    {
        const auto actor = Components::Actors::GetActor(id);
        if (!actor)
            return;
        const auto timing = Components::Actors::GetPathTiming(*actor);
        auto copy = Components::Actors::MakeBeside(*actor, left ? BESIDE : -BESIDE);
        copy.name = Components::Actors::MakeName(actor->name);
        const auto added = Components::Actors::Add(std::move(copy));
        // (planned along its own path, which is longer or shorter round a corner, arriving when the other does)
        if (auto placed = Components::Actors::GetActor(added);
            placed && placed->movement == Types::ActorMovement::Path && placed->path.size() >= 2)
        {
            Components::Actors::FitOnlyWalk(*placed);
            if (timing.arrival)
                Components::Actors::ArriveAt(*placed, *timing.arrival);
            Components::Actors::Update(*placed, true);
        }
        Components::Actors::Select(added);
    }

    void RunWith(std::uint32_t id, std::uint32_t leaderId)
    {
        auto actor = Components::Actors::GetActor(id);
        const auto leader = Components::Actors::GetActor(leaderId);
        if (!actor || !leader)
            return;
        // (on the side of the leader it stands on)
        const auto from = Components::Actors::EvaluatePlacement(*leader, GetTick());
        const auto way = glm::radians(from.yaw);
        const auto side =
            glm::dot(glm::vec2(actor->position - from.position), glm::vec2(-std::sin(way), std::cos(way)));
        Components::Actors::RunWith(*actor, *leader, side >= 0.0f ? BESIDE : -BESIDE);
        Change(*actor, std::format("{} runs with {}", actor->name, leader->name));
    }

    void Delete(std::uint32_t id)
    {
        if (const auto actor = Components::Actors::GetActor(id))
        {
            Components::Actors::Remove(id);
            Toasts::Show(actor->name + " deleted", true);
        }
    }

    void Update()
    {
        // paths changed: their walks fitted and their transitions planned again, once nothing is dragged or typed
        if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyMouseDown())
            Components::Actors::RunPendingReplans();
        const auto fitted = Components::Actors::GetWalkFittedActor();
        if (fitted &&
            (!GFX::GraphicsManager::Get().IsPlacingPathPoints() || Components::Actors::GetSelected() != fitted))
            Components::Actors::StopFittingWalk();
    }
}  // namespace IWXMVM::UI::ActorActions
