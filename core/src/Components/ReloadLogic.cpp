#include "StdInclude.hpp"
#include "ReloadLogic.hpp"

#include "Components/PoseLogic.hpp"

namespace IWXMVM::ReloadLogic
{
    using Types::ReloadHand;
    using Types::ReloadKind;
    using Types::ReloadSource;

    namespace
    {
        constexpr std::array<std::pair<ReloadKind, std::string_view>, 6> KIND_NAMES{{
            {ReloadKind::None, "none"},
            {ReloadKind::Magazine, "magazine"},
            {ReloadKind::DroppedMagazine, "dropped"},
            {ReloadKind::Box, "box"},
            {ReloadKind::Shells, "shells"},
            {ReloadKind::Rocket, "rocket"},
        }};
        constexpr std::array<std::pair<ReloadHand, std::string_view>, 3> HAND_NAMES{{
            {ReloadHand::HandTag, "tag"},
            {ReloadHand::LeftWrist, "left"},
            {ReloadHand::RightWrist, "right"},
        }};

        template <typename T, std::size_t N>
        std::optional<T> FindByName(const std::array<std::pair<T, std::string_view>, N>& names,
                                    const nlohmann::json& json)
        {
            if (!json.is_string())
                return std::nullopt;
            const auto name = json.get<std::string>();
            for (const auto& [value, valueName] : names)
                if (valueName == name)
                    return value;
            return std::nullopt;
        }

        template <typename T, std::size_t N>
        std::string_view GetName(const std::array<std::pair<T, std::string_view>, N>& names, T value)
        {
            for (const auto& [named, name] : names)
                if (named == value)
                    return name;
            return {};
        }
    }  // namespace

    std::string_view GetKindName(ReloadKind kind)
    {
        return GetName(KIND_NAMES, kind);
    }

    std::string_view GetHandName(ReloadHand hand)
    {
        return GetName(HAND_NAMES, hand);
    }

    std::string_view GetSourceName(ReloadSource source)
    {
        switch (source)
        {
            case ReloadSource::Override:
                return "override";
            case ReloadSource::Curated:
                return "curated";
            case ReloadSource::ClipNotetracks:
                return "clip notetracks";
            case ReloadSource::SoundNotetracks:
                return "sounds";
            case ReloadSource::Geometry:
                return "geometry";
            default:
                return "none";
        }
    }

    std::string_view GetMomentName(Moment moment)
    {
        switch (moment)
        {
            case Moment::Out:
                return "out";
            case Moment::Swap:
                return "swap";
            default:
                return "in";
        }
    }

    const std::optional<Types::ReloadMoment>& GetMoment(const Types::ReloadProfile& profile, Moment moment)
    {
        return moment == Moment::Out ? profile.out : moment == Moment::Swap ? profile.swap : profile.in;
    }

    std::optional<float>& GetMoment(ProfileEntry& entry, Moment moment)
    {
        return moment == Moment::Out ? entry.out : moment == Moment::Swap ? entry.swap : entry.in;
    }

    std::optional<float> GetMoment(const ProfileEntry& entry, Moment moment)
    {
        return moment == Moment::Out ? entry.out : moment == Moment::Swap ? entry.swap : entry.in;
    }

    float PlaceMoment(const Types::ReloadProfile& profile, Moment moment, float time)
    {
        // the known moments next to it
        const auto timeOf = [&](Moment other) {
            const auto& known = GetMoment(profile, other);
            return known ? std::optional(known->time) : std::nullopt;
        };
        const auto either = [&](Moment first, Moment second) {
            const auto time = timeOf(first);
            return time ? time : timeOf(second);
        };
        const auto before = moment == Moment::Out    ? std::nullopt
                            : moment == Moment::Swap ? timeOf(Moment::Out)
                                                     : either(Moment::Swap, Moment::Out);
        const auto after = moment == Moment::In     ? std::nullopt
                           : moment == Moment::Swap ? timeOf(Moment::In)
                                                    : either(Moment::Swap, Moment::In);
        const auto low = before ? *before + MIN_MOMENT_GAP : 0.0f;
        const auto high = after ? *after - MIN_MOMENT_GAP : 1.0f;
        if (low > high)
            return std::clamp((low + high) / 2.0f, 0.0f, 1.0f);
        return std::clamp(time, std::max(low, 0.0f), std::min(high, 1.0f));
    }

    ProfileEntry WithKind(ProfileEntry entry, std::optional<ReloadKind> kind, const Types::ReloadProfile& profile)
    {
        entry.kind = kind;
        const bool magazine = kind == ReloadKind::Magazine || kind == ReloadKind::DroppedMagazine;
        if (magazine && (!profile.out || !profile.in) && !entry.out && !entry.in)
        {
            entry.out = DEFAULT_MOMENTS[0];
            entry.swap = DEFAULT_MOMENTS[1];
            entry.in = DEFAULT_MOMENTS[2];
        }
        return entry;
    }

    bool SaysReload(const Types::ReloadProfile& profile)
    {
        return profile.kindSource != ReloadSource::None || profile.out || profile.swap || profile.in;
    }

    nlohmann::json EntryToJson(const ProfileEntry& entry)
    {
        auto json = nlohmann::json::object();
        if (entry.kind)
            json["kind"] = GetKindName(*entry.kind);
        if (entry.hand)
            json["hand"] = GetHandName(*entry.hand);
        for (const auto& [name, moment] : {std::pair("out", entry.out), {"swap", entry.swap}, {"in", entry.in}})
            if (moment)
                json[name] = *moment;
        return json;
    }

    ProfileEntry EntryFromJson(const nlohmann::json& json)
    {
        ProfileEntry entry;
        if (!json.is_object())
            return entry;
        if (const auto it = json.find("kind"); it != json.end())
            entry.kind = FindByName(KIND_NAMES, *it);
        if (const auto it = json.find("hand"); it != json.end())
            entry.hand = FindByName(HAND_NAMES, *it);
        for (const auto& [name, moment] : {std::pair("out", &entry.out), {"swap", &entry.swap}, {"in", &entry.in}})
            if (const auto it = json.find(name); it != json.end() && it->is_number())
                *moment = std::clamp(it->get<float>(), 0.0f, 1.0f);
        return entry;
    }

    ProfileTable TableFromJson(const nlohmann::json& root)
    {
        ProfileTable table;
        if (!root.is_object())
            return table;
        const auto profiles = root.find("profiles");
        if (profiles == root.end() || !profiles->is_object())
            return table;
        for (const auto& [animation, entry] : profiles->items())
            table.emplace(animation, EntryFromJson(entry));
        return table;
    }

    std::optional<NotetrackMoments> FindClipMoments(std::span<const Types::ActorNotetrack> notetracks)
    {
        // what SP scripts do at them: "drop clip" throws the gun's magazine away, "attach clip left" (or right) holds a
        // new one in the hand and "detach clip ..." puts it on the gun, as does "reload done" when there is no detach;
        // shotguns load a shell at each "load_shell"
        NotetrackMoments moments{.source = ReloadSource::ClipNotetracks};
        std::optional<float> drop, attach;
        for (const auto& notetrack : notetracks)
        {
            const std::string_view name = notetrack.name;
            if (name == "load_shell")
                moments.inserts.push_back(notetrack.time);
            else if (name == "drop clip" && !drop && !attach)
                drop = notetrack.time;
            else if (name.starts_with("attach clip") && !attach)
                attach = notetrack.time;
            else if ((name.starts_with("detach clip") || name == "reload done") && !moments.in && (drop || attach))
                moments.in = notetrack.time;
        }
        if (!moments.inserts.empty())
        {
            moments.kind = ReloadKind::Shells;
            moments.in = moments.inserts.back();
            return moments;
        }
        if (!drop && !attach)
            return std::nullopt;
        moments.kind = drop ? ReloadKind::DroppedMagazine : ReloadKind::Magazine;
        moments.out = drop ? drop : attach;
        if (drop)
            moments.swap = attach;
        return moments;
    }

    std::optional<NotetrackMoments> FindSoundMoments(std::span<const Types::ActorNotetrack> notetracks)
    {
        NotetrackMoments magazine{.kind = ReloadKind::Magazine, .source = ReloadSource::SoundNotetracks};
        NotetrackMoments box{.kind = ReloadKind::Box, .source = ReloadSource::SoundNotetracks};
        NotetrackMoments shells{.kind = ReloadKind::Shells, .source = ReloadSource::SoundNotetracks};
        NotetrackMoments rocket{.kind = ReloadKind::Rocket, .source = ReloadSource::SoundNotetracks};
        std::optional<float> lmgOpen;
        for (const auto& notetrack : notetracks)
        {
            const auto sound = ActorLogic::GetNotetrackSound(notetrack.name);
            const auto time = notetrack.time;
            if (sound.empty())
                continue;
            if (sound.find("mag_out") != std::string_view::npos && !magazine.out)
                magazine.out = time;
            else if (sound.find("mag_in") != std::string_view::npos && magazine.out && !magazine.in)
                magazine.in = time;
            else if (sound.find("shell_in") != std::string_view::npos)
                shells.inserts.push_back(time);
            else if (sound.starts_with("wpn_lmg") && sound.ends_with("_open") && !lmgOpen)
                lmgOpen = time;
            else if (sound.starts_with("wpn_lmg") && sound.ends_with("_box_off") && !box.out)
                box.out = time;
            else if (sound.starts_with("wpn_lmg") && sound.ends_with("_place_belt") && !box.in)
                box.in = time;
            else if (sound.starts_with("wpn_rocket"))
                rocket.in = time;
        }

        if (rocket.in)
            return rocket;
        if (box.in)
        {
            // (the stand and crouch reloads don't sound the box coming off: from opening the cover)
            if (!box.out || *box.out > *box.in)
                box.out = lmgOpen;
            return box;
        }
        if (!shells.inserts.empty())
        {
            shells.in = shells.inserts.back();
            return shells;
        }
        if (magazine.out)
            return magazine;
        return std::nullopt;
    }

    std::optional<float> FindFarthest(const Curve& curve, float from, float to)
    {
        std::optional<std::size_t> farthest;
        for (std::size_t i = 0; i < curve.times.size(); ++i)
        {
            if (curve.times[i] <= from || curve.times[i] >= to)
                continue;
            if (!farthest || curve.distances[i] > curve.distances[*farthest])
                farthest = i;
        }
        return farthest ? std::optional(curve.times[*farthest]) : std::nullopt;
    }

    std::optional<Trip> FindTrip(const Curve& curve)
    {
        const auto& d = curve.distances;
        const auto& t = curve.times;
        const auto n = d.size();
        if (n < 3 || t.size() != n)
            return std::nullopt;
        const auto peak = static_cast<std::size_t>(std::ranges::max_element(d) - d.begin());
        if (d[peak] < MIN_TRIP)
            return std::nullopt;

        // the nearest points around the trip
        const auto isNearest = [&](std::size_t i) {
            return (i == 0 || d[i] <= d[i - 1]) && (i + 1 == n || d[i] <= d[i + 1]) && d[i] <= NEAR_MAGAZINE;
        };
        std::optional<std::size_t> before, after;
        for (auto i = peak; i-- > 0 && !before;)
            if (isNearest(i))
                before = i;
        for (auto i = peak + 1; i < n && !after; ++i)
            if (isNearest(i))
                after = i;
        if (!before || !after)
            return std::nullopt;
        return Trip{.out = t[*before], .swap = t[peak], .in = t[*after]};
    }

    std::optional<glm::mat4> GetHand(const ReloadBones& bones, ReloadHand hand)
    {
        switch (hand)
        {
            case ReloadHand::HandTag:
                return bones.handTag;
            case ReloadHand::RightWrist:
                return bones.rightWrist;
            default:
                return bones.leftWrist;
        }
    }

    glm::mat4 GetMagazineOnGun(const ReloadBones& bones, const Types::WeaponRig& rig)
    {
        return bones.gun * rig.magazine;
    }

    namespace
    {
        // how far tag_inhand may move relative to a wrist while it moves with it, and how far it moves relative to the
        // gun at least while it is in a hand rather than on the gun
        constexpr float HAND_LENGTH = 10.0f;
        constexpr float TAG_STILL = 1.5f;
        constexpr float TAG_STILL_FRACTION = 0.75f;
        constexpr float TAG_OFF_GUN = 5.0f;

        glm::vec3 Relative(const glm::mat4& to, const glm::mat4& transform)
        {
            return glm::vec3(glm::inverse(to) * transform[3]);
        }
    }  // namespace

    bool MovesWith(std::span<const ReloadBones> stretch, ReloadHand wrist)
    {
        if (stretch.size() < 2)
            return false;
        const auto& middle = stretch[stretch.size() / 2];
        if (!middle.handTag)
            return false;
        const auto wristAt = [&](const ReloadBones& bones) { return *GetHand(bones, wrist); };
        const auto onWrist = Relative(wristAt(middle), *middle.handTag);
        const auto onGun = Relative(middle.gun, *middle.handTag);
        if (glm::length(onWrist) > HAND_LENGTH)
            return false;
        std::size_t still = 0;
        float offGun = 0.0f;
        for (const auto& bones : stretch)
        {
            if (!bones.handTag)
                return false;
            still += glm::distance(Relative(wristAt(bones), *bones.handTag), onWrist) <= TAG_STILL ? 1 : 0;
            offGun = std::max(offGun, glm::distance(Relative(bones.gun, *bones.handTag), onGun));
        }
        return still >= TAG_STILL_FRACTION * static_cast<float>(stretch.size()) && offGun >= TAG_OFF_GUN;
    }

    glm::mat4 GetMagazineModel(const glm::mat4& magazineBone, const Types::WeaponRig& rig)
    {
        return magazineBone * glm::inverse(rig.magazine);
    }

    namespace
    {
        constexpr float SAMPLES_PER_SECOND = 60.0f;
        constexpr std::size_t MIN_SAMPLES = 33;
        constexpr std::size_t MAX_SAMPLES = 601;
        // seconds of the animation over which a falling magazine's speed is measured
        constexpr float VELOCITY_SECONDS = 0.033f;
        // how near tag_inhand must hold the magazine to where the gun has it for the grip to be the authored one
        constexpr float AUTHORED_GRIP_TOLERANCE = 0.1f;
        constexpr float GRIP_COMPARED_LENGTH = 10.0f;
        constexpr float DROPPED_SPIN = 300.0f;
        constexpr float PULLED_SPIN = -240.0f;

        bool IsMagazineKind(ReloadKind kind)
        {
            return kind == ReloadKind::Magazine || kind == ReloadKind::DroppedMagazine;
        }

        // the moments of a profile as fields, to fill in order
        struct Moments
        {
            std::optional<Types::ReloadMoment>* out;
            std::optional<Types::ReloadMoment>* swap;
            std::optional<Types::ReloadMoment>* in;

            // sets a moment if it is still unknown and comes in order with those known: out, swap, in
            void Fill(std::optional<Types::ReloadMoment>* moment, std::optional<float> time, ReloadSource source) const
            {
                if (!time || moment->has_value())
                    return;
                const auto before = [](const std::optional<Types::ReloadMoment>& m, float t) {
                    return !m || m->time < t;
                };
                const auto after = [](const std::optional<Types::ReloadMoment>& m, float t) {
                    return !m || m->time > t;
                };
                const bool fits = moment == out    ? after(*swap, *time) && after(*in, *time)
                                  : moment == swap ? before(*out, *time) && after(*in, *time)
                                                   : before(*out, *time) && before(*swap, *time);
                if (fits)
                    *moment = Types::ReloadMoment{*time, source};
            }
        };

        void AddNote(Types::ReloadProfile& profile, std::string_view note)
        {
            if (!profile.note.empty())
                profile.note += " ";
            profile.note += note;
        }
    }  // namespace

    bool IsReloadName(std::string_view animation)
    {
        return animation.find("reload") != std::string_view::npos;
    }

    Types::ReloadProfile MakeProfile(const ProfileInput& input)
    {
        Types::ReloadProfile profile;
        const Moments moments{&profile.out, &profile.swap, &profile.in};
        if (input.death && !input.override)
        {
            AddNote(profile, "A death doesn't reload.");
            return profile;
        }

        // what the user and the table say
        for (const auto& [entry, source] :
             {std::pair(input.override, ReloadSource::Override), {input.curated, ReloadSource::Curated}})
        {
            if (!entry)
                continue;
            if (entry->kind && profile.kindSource == ReloadSource::None)
            {
                profile.kind = *entry->kind;
                profile.kindSource = source;
            }
            if (entry->hand && profile.handSource == ReloadSource::None)
            {
                profile.hand = *entry->hand;
                profile.handSource = source;
            }
            moments.Fill(&profile.out, entry->out, source);
            moments.Fill(&profile.swap, entry->swap, source);
            moments.Fill(&profile.in, entry->in, source);
        }

        // what the notetracks say
        for (const auto& found : {FindClipMoments(input.notetracks), FindSoundMoments(input.notetracks)})
        {
            if (!found)
                continue;
            // (a launcher's reload has clip notetracks for its rocket)
            if (profile.kindSource == ReloadSource::None ||
                (found->kind == ReloadKind::Rocket && profile.kindSource == ReloadSource::ClipNotetracks))
            {
                profile.kind = found->kind;
                profile.kindSource = found->source;
            }
            moments.Fill(&profile.out, found->out, found->source);
            moments.Fill(&profile.swap, found->swap, found->source);
            moments.Fill(&profile.in, found->in, found->source);
            if (profile.inserts.empty())
                profile.inserts = found->inserts;
        }

        // where the hand goes tells when a reload's magazine moves, not whether an animation is one
        if (!IsReloadName(input.animation) && !input.override && !SaysReload(profile))
        {
            AddNote(profile, "Not a reload: its name, notetracks and Reload setting say nothing of one.");
            return profile;
        }
        if (profile.kindSource != ReloadSource::None && !IsMagazineKind(profile.kind))
        {
            AddNote(profile, profile.kind == ReloadKind::None
                                 ? "Turned off."
                                 : "LMG boxes, shotgun shells and rockets aren't moved yet.");
            return profile;
        }
        const auto& rig = input.rig;
        if (!rig || rig->magazineBone.empty())
        {
            AddNote(profile, rig ? "The gun has no magazine of its own." : "There is no gun in the hand.");
            profile.kind = ReloadKind::None;
            profile.kindSource = ReloadSource::None;
            return profile;
        }
        profile.magazineBone = rig->magazineBone;

        // where the bones are over the animation
        const auto count = std::clamp(static_cast<std::size_t>(std::ceil(input.duration * SAMPLES_PER_SECOND)) + 1,
                                      MIN_SAMPLES, MAX_SAMPLES);
        std::vector<float> times;
        std::vector<ReloadBones> samples;
        for (std::size_t i = 0; i < count && input.bones; ++i)
        {
            const auto time = static_cast<float>(i) / static_cast<float>(count - 1);
            const auto bones = input.bones(time);
            if (!bones)
                break;
            times.push_back(time);
            samples.push_back(*bones);
        }
        if (samples.size() != count)
        {
            samples.clear();
            AddNote(profile,
                    "The hands aren't worked out for this animation (none are for reloads of 256 frames or more "
                    "yet), so the magazine stays on the gun.");
        }

        // the hand that fetches the magazine: the wrist that moves away from the gun (the other holds it), unless
        // said
        const auto curveOf = [&](ReloadHand hand) {
            Curve curve{.times = times};
            for (const auto& bones : samples)
            {
                const auto at = GetHand(bones, hand);
                curve.distances.push_back(
                    at ? glm::distance(glm::vec3((*at)[3]), glm::vec3(GetMagazineOnGun(bones, *rig)[3]))
                       : std::numeric_limits<float>::max());
            }
            return curve;
        };
        const auto range = [](const Curve& curve) {
            const auto [low, high] = std::ranges::minmax(curve.distances);
            return high - low;
        };
        std::optional<Curve> curve;
        ReloadHand wrist = ReloadHand::LeftWrist;
        if (!samples.empty())
        {
            if (range(curveOf(ReloadHand::RightWrist)) > range(curveOf(ReloadHand::LeftWrist)))
                wrist = ReloadHand::RightWrist;
            auto fetching = profile.handSource != ReloadSource::None ? profile.hand : wrist;
            if (fetching == ReloadHand::HandTag &&
                !std::ranges::all_of(samples, [](const ReloadBones& b) { return b.handTag.has_value(); }))
                fetching = wrist;
            curve = curveOf(fetching);
        }

        // what the hand's trip tells, for the moments still unknown
        if (curve)
        {
            if (!profile.out || !profile.in)
            {
                if (const auto trip = FindTrip(*curve))
                {
                    moments.Fill(&profile.out, trip->out, ReloadSource::Geometry);
                    moments.Fill(&profile.in, trip->in, ReloadSource::Geometry);
                    moments.Fill(&profile.swap, trip->swap, ReloadSource::Geometry);
                }
            }
            if (!profile.swap && profile.out && profile.in)
                moments.Fill(&profile.swap, FindFarthest(*curve, profile.out->time, profile.in->time),
                             ReloadSource::Geometry);
        }
        if (!profile.out || !profile.in)
        {
            AddNote(profile, "Nothing tells when the magazine would leave the gun and go back in.");
            profile.kind = ReloadKind::None;
            profile.kindSource = ReloadSource::None;
            return profile;
        }
        if (profile.kindSource == ReloadSource::None)
        {
            profile.kind = ReloadKind::Magazine;
            profile.kindSource = profile.out->source;
        }
        if (samples.empty())
            return profile;

        // the hand that holds it: tag_inhand when it moves with a wrist while the magazine is carried, as SP animations
        // hold clip models on it, else the wrist. Notetracks naming a hand aren't trusted: the mirrored SP corner
        // reloads (cornercrl_reloada) say right where the left hand reloads.
        const auto stretch = [&](float from, float to) {
            std::vector<ReloadBones> inside;
            for (std::size_t i = 0; i < samples.size(); ++i)
                if (times[i] >= from && times[i] <= to)
                    inside.push_back(samples[i]);
            return inside;
        };
        if (profile.handSource == ReloadSource::None)
        {
            const auto carried = stretch(profile.out->time, profile.in->time);
            const auto other = wrist == ReloadHand::LeftWrist ? ReloadHand::RightWrist : ReloadHand::LeftWrist;
            profile.hand = MovesWith(carried, wrist) || MovesWith(carried, other) ? ReloadHand::HandTag : wrist;
            profile.handSource = ReloadSource::Geometry;
        }

        const auto bonesAt = [&](float time) -> std::optional<ReloadBones> { return input.bones(time); };
        const auto handAt = [&](float time) -> std::optional<glm::mat4> {
            const auto bones = bonesAt(time);
            return bones ? GetHand(*bones, profile.hand) : std::nullopt;
        };
        const auto magazineAt = [&](float time) -> std::optional<glm::mat4> {
            const auto bones = bonesAt(time);
            return bones ? std::optional(GetMagazineOnGun(*bones, *rig)) : std::nullopt;
        };

        // a magazine the hand isn't at as it leaves the gun falls from it instead
        if (profile.kind == ReloadKind::Magazine && profile.kindSource != ReloadSource::Override &&
            profile.kindSource != ReloadSource::Curated)
        {
            const auto hand = handAt(profile.out->time);
            const auto magazine = magazineAt(profile.out->time);
            if (hand && magazine && glm::distance(glm::vec3((*hand)[3]), glm::vec3((*magazine)[3])) > NEAR_MAGAZINE)
            {
                profile.kind = ReloadKind::DroppedMagazine;
                profile.kindSource = ReloadSource::Geometry;
            }
        }

        // how the hand holds them: as it is to the magazine on the gun as it takes it out or puts it in
        const auto gripAt = [&](float time) -> std::optional<Types::ReloadGrip> {
            const auto hand = handAt(time);
            const auto magazine = magazineAt(time);
            if (!hand || !magazine)
                return std::nullopt;
            Types::ReloadGrip grip{.magazine = glm::inverse(*hand) * *magazine};
            if (profile.hand == ReloadHand::HandTag &&
                PoseLogic::GetTransformDistance(grip.magazine, glm::mat4(1.0f), GRIP_COMPARED_LENGTH) <=
                    AUTHORED_GRIP_TOLERANCE)
                grip = {Types::ReloadGrip::Kind::Authored, glm::mat4(1.0f)};
            return grip;
        };
        if (profile.kind == ReloadKind::Magazine)
            profile.oldGrip = gripAt(profile.out->time);
        profile.newGrip = gripAt(profile.in->time);
        if (!profile.newGrip || (profile.kind == ReloadKind::Magazine && !profile.oldGrip))
        {
            AddNote(profile, "The hand couldn't be found at the magazine.");
            return profile;
        }

        // the old one falls from the gun as it leaves it, or from the hand as it swaps it, as fast as that moves
        const auto oldAt = [&](float time) -> std::optional<glm::mat4> {
            if (profile.kind == ReloadKind::DroppedMagazine)
                return magazineAt(time);
            const auto hand = handAt(time);
            return hand ? std::optional(*hand * profile.oldGrip->magazine) : std::nullopt;
        };
        const auto fallTime = profile.kind == ReloadKind::DroppedMagazine ? std::optional(profile.out->time)
                              : profile.swap                              ? std::optional(profile.swap->time)
                                                                          : std::nullopt;
        if (const auto start = fallTime ? oldAt(*fallTime) : std::nullopt)
        {
            Types::ReloadFall fall{.time = *fallTime,
                                   .magazine = *start,
                                   .spin = profile.kind == ReloadKind::DroppedMagazine ? DROPPED_SPIN : PULLED_SPIN,
                                   .fromGun = profile.kind == ReloadKind::DroppedMagazine};
            const auto middleAt = [&](const glm::mat4& magazine) {
                return glm::vec3(GetMagazineModel(magazine, *rig) *
                                 glm::vec4(glm::vec3(rig->magazine[3]) + MAGAZINE_MIDDLE, 1.0f));
            };
            const auto step = input.duration > 0.0f ? VELOCITY_SECONDS / input.duration : 0.0f;
            const auto later = std::min(*fallTime + step, 1.0f);
            if (const auto next = later > *fallTime ? oldAt(later) : std::nullopt)
                fall.velocity = (middleAt(*next) - middleAt(*start)) / ((later - *fallTime) * input.duration);
            if (fall.fromGun)
                fall.push = glm::mat3(GetMagazineModel(*start, *rig)) * glm::vec3(0.0f, 0.0f, -MAGAZINE_PUSH);
            profile.fall = fall;
        }
        return profile;
    }

    std::optional<ActorLogic::MagazineFall> PlaceFall(const Types::ReloadProfile& profile, const Types::WeaponRig& rig,
                                                      const glm::mat4& placement, float speed,
                                                      const std::function<float(glm::vec3 point)>& groundBelow,
                                                      const std::optional<glm::mat4>& drawnMagazine)
    {
        if (!profile.fall)
            return std::nullopt;
        ActorLogic::MagazineFall fall;
        fall.start = placement * GetMagazineModel(profile.fall->magazine, rig);
        // where the game draws it, turning its motion with it
        auto turn = glm::mat3(placement);
        if (drawnMagazine)
        {
            const auto drawn = GetMagazineModel(*drawnMagazine, rig);
            turn = glm::mat3(drawn) * glm::transpose(glm::mat3(fall.start)) * turn;
            fall.start = drawn;
        }
        fall.middle = glm::vec3(rig.magazine[3]) + MAGAZINE_MIDDLE;
        fall.velocity = turn * (profile.fall->velocity * std::max(speed, 0.0f));
        if (glm::length(fall.velocity) > MAX_MAGAZINE_SPEED)
            fall.velocity = glm::normalize(fall.velocity) * MAX_MAGAZINE_SPEED;
        fall.velocity += turn * profile.fall->push;
        fall.spin = profile.fall->spin;

        // the ground below where it starts, then below where it lands on that
        const auto middle = glm::vec3(fall.start * glm::vec4(fall.middle, 1.0f));
        fall.ground = groundBelow(middle);
        const auto landing = ActorLogic::GetFallingMiddle(fall, ActorLogic::GetLandingTime(fall));
        fall.ground = groundBelow(glm::vec3(landing.x, landing.y, middle.z));
        return fall;
    }

    std::optional<ActorLogic::ReloadMoments> GetMoments(const Types::ReloadProfile& profile)
    {
        if (!IsMagazineKind(profile.kind) || !profile.out || !profile.in || !profile.newGrip ||
            (profile.kind == ReloadKind::Magazine && !profile.oldGrip))
            return std::nullopt;
        return ActorLogic::ReloadMoments{
            .out = profile.out->time,
            .in = profile.in->time,
            .swap = profile.swap ? std::optional(profile.swap->time) : std::nullopt,
            .dropped = profile.kind == ReloadKind::DroppedMagazine,
        };
    }

    std::string_view GetHandBone(ReloadHand hand)
    {
        switch (hand)
        {
            case ReloadHand::HandTag:
                return "tag_inhand";
            case ReloadHand::RightWrist:
                return "j_wrist_ri";
            default:
                return "j_wrist_le";
        }
    }

    Magazines GetMagazines(std::span<const ActorLogic::Reload> reloads, const ProfileLookup& profiles,
                           const Types::WeaponRig& rig, std::uint32_t tick, std::size_t maxFallen)
    {
        Magazines magazines;
        if (reloads.empty())
            return magazines;

        // the last is the only one that may still be playing: a reload stops where the next starts
        const auto& last = reloads.back();
        if (const auto profile = profiles(*last.clip))
        {
            magazines.state = ActorLogic::GetReloadState(last, tick);
            magazines.hand = profile->hand;
            const auto& grip = magazines.state.held == ActorLogic::HeldMagazine::Old   ? profile->oldGrip
                               : magazines.state.held == ActorLogic::HeldMagazine::New ? profile->newGrip
                                                                                       : std::nullopt;
            if (grip)
                magazines.held = GetMagazineModel(grip->magazine, rig);
            else
                magazines.state.held = ActorLogic::HeldMagazine::None;
        }

        // those that fell, of the last few reloads (the one playing may not have let its own go yet)
        const auto first = reloads.size() > maxFallen + 1 ? reloads.size() - maxFallen - 1 : std::size_t{0};
        for (auto i = reloads.size(); i-- > first && magazines.fallen.size() < maxFallen;)
        {
            const auto start = ActorLogic::GetFallStart(reloads[i]);
            if (!start || *start > tick)
                continue;
            auto profile = profiles(*reloads[i].clip);
            if (profile && profile->fall)
                magazines.fallen.push_back({&reloads[i], std::move(profile), *start});
        }
        return magazines;
    }
}  // namespace IWXMVM::ReloadLogic
