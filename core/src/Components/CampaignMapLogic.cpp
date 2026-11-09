#include "StdInclude.hpp"
#include "CampaignMapLogic.hpp"

#include "Utilities/Deflate.hpp"
#include "Utilities/Inflate.hpp"
#include <numeric>
#include "nlohmann/json.hpp"

namespace IWXMVM::Components::CampaignMapLogic
{
    namespace
    {
        constexpr std::array<std::pair<Teamset, std::string_view>, 4> TEAMSETS{{
            {Teamset::UrbanSpecOps, "urbanspecops"},
            {Teamset::JungleMarines, "junglemarines"},
            {Teamset::WinterSpecOps, "winterspecops"},
            {Teamset::Cubans, "cubans"},
        }};

        // what multiplayer spawns of a campaign level's entities (parked vehicles become script models)
        const std::set<std::string, std::less<>> KEPT_CLASSES{"worldspawn",    "script_model", "script_brushmodel",
                                                              "script_origin", "script_struct", "light",
                                                              "script_vehicle", "misc_turret"};
        // keys left out of what is kept: what made a model destructible or a vehicle drive (and the nodes it went
        // along), which multiplayer has no assets for, and the models a light switches between as it flickers, which
        // multiplayer's client script (_lights.csc) sets without the server having precached them: "model not
        // precached" ended flashpoint's load
        const std::set<std::string, std::less<>> DROPPED_KEYS{
            "destructibledef",       "vehicletype",          "script_vehicleride",
            "script_light_model",    "script_light_on_model", "script_light_off_model"};
        const std::set<std::string, std::less<>> DROPPED_VEHICLE_KEYS{"target", "spawnflags"};
        // the spawner flag of a script_vehicle, and the vehicle types that are never parked (see IsParkedVehicle)
        constexpr int VEHICLE_SPAWNER = 2;
        constexpr std::array<std::string_view, 3> NOT_PARKED_TYPES{"heli_", "plane_", "misc_"};

        // the classes of every game type's spawn points, each put at each spawn point
        constexpr std::array<std::string_view, 19> SPAWN_CLASSES{
            "mp_dm_spawn",          "mp_tdm_spawn",           "mp_tdm_spawn_allies_start",
            "mp_tdm_spawn_axis_start", "mp_dom_spawn",        "mp_dom_spawn_allies_start",
            "mp_dom_spawn_axis_start", "mp_sab_spawn_allies", "mp_sab_spawn_axis",
            "mp_sab_spawn_allies_start", "mp_sab_spawn_axis_start", "mp_ctf_spawn_allies",
            "mp_ctf_spawn_axis",    "mp_ctf_spawn_allies_start", "mp_ctf_spawn_axis_start",
            "mp_sd_spawn_attacker", "mp_sd_spawn_defender",   "mp_dem_spawn_attacker",
            "mp_dem_spawn_defender"};

        std::optional<glm::vec3> ReadVector(const std::string* text)
        {
            if (!text)
                return std::nullopt;
            glm::vec3 value{0.0f};
            std::istringstream numbers(*text);
            if (!(numbers >> value.x >> value.y >> value.z))
                return std::nullopt;
            return value;
        }

        // a number as the entity string has them: no trailing zeros
        std::string FormatNumber(float value)
        {
            auto text = std::format("{:.3f}", value);
            text.erase(text.find_last_not_of('0') + 1);
            if (text.back() == '.')
                text.pop_back();
            return text == "-0" ? "0" : text;
        }

        std::string FormatVector(const glm::vec3& value)
        {
            return FormatNumber(value.x) + " " + FormatNumber(value.y) + " " + FormatNumber(value.z);
        }
    }  // namespace

    std::string_view GetTeamsetName(Teamset teamset)
    {
        return std::ranges::find(TEAMSETS, teamset, &std::pair<Teamset, std::string_view>::first)->second;
    }

    std::optional<Teamset> FindTeamset(std::string_view name)
    {
        const auto found = std::ranges::find(TEAMSETS, name, &std::pair<Teamset, std::string_view>::second);
        return found != TEAMSETS.end() ? std::optional(found->first) : std::nullopt;
    }

    std::string_view GetModeName(Mode mode)
    {
        return mode == Mode::Zombies ? "Zombies map" : "campaign level";
    }

    std::string GetMissingMapRefusal(const Level& level)
    {
        return std::format("It was recorded on {} ({}), a {} this game folder doesn't have. Add the map's files to the "
                           "game folder to play it.",
                           level.title, level.name, GetModeName(level.mode));
    }

    std::string GetKitZone(Teamset teamset)
    {
        return std::string(KIT_ZONE_PREFIX) + std::string(GetTeamsetName(teamset));
    }

    const Level* Catalog::FindLevel(std::string_view name) const
    {
        const auto found = std::ranges::find(levels, name, &Level::name);
        return found != levels.end() ? &*found : nullptr;
    }

    Catalog ReadCatalog(std::string_view json)
    {
        const auto root = nlohmann::json::parse(json, nullptr, false);
        if (root.is_discarded() || !root.is_object())
            throw std::runtime_error("campaign-maps.json isn't JSON");
        if (root.value("version", 0) != 1)
            throw std::runtime_error("campaign-maps.json isn't of version 1");
        const auto teamsetOf = [](const nlohmann::json& node) {
            const auto teamset = FindTeamset(node.at("teamset").get<std::string>());
            if (!teamset)
                throw std::runtime_error("campaign-maps.json names a teamset there is none of");
            return *teamset;
        };

        Catalog catalog;
        for (const auto& node : root.at("levels"))
        {
            auto& level = catalog.levels.emplace_back();
            level.name = node.at("name").get<std::string>();
            level.title = node.value("title", level.name);
            const auto mode = node.value("mode", "campaign");
            if (mode != "campaign" && mode != "zombies")
                throw std::runtime_error("campaign-maps.json has a level of a mode there is none of");
            level.mode = mode == "zombies" ? Mode::Zombies : Mode::Campaign;
            level.teamset = teamsetOf(node);
            level.zones = node.value("zones", std::vector<std::string>{});
            level.fog = node.value("fog", "");
            level.vision = node.value("vision", "");
            if (const auto added = node.find("new"); added != node.end())
            {
                level.newImages = added->value("image", 0u);
                level.newMaterials = added->value("material", 0u);
                level.newModels = added->value("xmodel", 0u);
                level.newEffects = added->value("fx", 0u);
            }
            for (const auto& [type, names] : node.at("world").items())
                for (const auto& name : names)
                    level.world[type].insert(name.get<std::string>());
            // [kind, x, y, z, pitch, yaw, roll, delay, loop]
            if (const auto effects = node.find("effects"); effects != node.end())
            {
                level.effectKinds = effects->at("names").get<std::vector<std::string>>();
                for (const auto& placed : effects->at("placed"))
                {
                    const auto kind = placed.at(0).get<std::uint32_t>();
                    if (placed.size() != 9 || kind >= level.effectKinds.size())
                        throw std::runtime_error("campaign-maps.json has an effect it can't read");
                    const auto at = [&](std::size_t i) { return placed.at(i).get<float>(); };
                    level.effects.push_back({kind, {at(1), at(2), at(3)}, {at(4), at(5), at(6)}, at(7),
                                             placed.at(8).get<int>() != 0});
                }
            }
            if (const auto menu = node.find("menu"); menu != node.end())
                level.menu = Level::MenuEntry{.order = menu->at("order").get<std::uint32_t>(),
                                              .name = menu->at("name").get<std::string>(),
                                              .description = menu->at("description").get<std::string>(),
                                              .picture = menu->value("picture", "")};
            if (const auto closed = node.find("closedDoors"); closed != node.end())
                for (const auto& name : *closed)
                    level.closedDoors.insert(name.get<std::string>());
        }
        if (const auto strings = root.find("strings"); strings != root.end())
            for (const auto& [key, text] : strings->items())
                catalog.strings.emplace(key, text.get<std::string>());
        if (const auto players = root.find("players"); players != root.end())
            for (const auto& [teamset, models] : players->items())
            {
                const auto found = FindTeamset(teamset);
                if (!found)
                    throw std::runtime_error("campaign-maps.json names a teamset there is none of");
                catalog.playerModels[*found] = models.get<std::vector<std::string>>();
            }
        return catalog;
    }

    const Level* FindLevelOfZone(const Catalog& catalog, std::string_view zone)
    {
        if (const auto level = catalog.FindLevel(zone))
            return level;
        // a language's twin: two letters and an underscore before the level's name
        if (zone.size() > 3 && zone[2] == '_')
            return catalog.FindLevel(zone.substr(3));
        return nullptr;
    }

    bool IsLoadedWith(const Level& level, std::string_view zone)
    {
        constexpr std::string_view PATCH = "_patch";
        const auto twinOf = [&](std::string_view name) {
            return zone == name || (zone.size() > 3 && zone[2] == '_' && zone.substr(3) == name) ||
                   (zone.size() == name.size() + PATCH.size() && zone.starts_with(name) && zone.ends_with(PATCH));
        };
        return std::ranges::any_of(level.zones, twinOf) ||
               (zone.size() == level.name.size() + PATCH.size() && zone.starts_with(level.name) &&
                zone.ends_with(PATCH));
    }

    bool IsLoadedWithAny(const Catalog& catalog, std::string_view zone)
    {
        return std::ranges::any_of(catalog.levels, [&](const Level& level) { return IsLoadedWith(level, zone); });
    }

    std::string GetWorldName(std::string_view level)
    {
        return std::format("maps/{}.d3dbsp", level);
    }

    std::string GetMultiplayerWorldName(std::string_view level)
    {
        return std::format("maps/mp/{}.d3dbsp", level);
    }

    std::vector<Entity> ParseEntities(std::string_view text)
    {
        std::vector<Entity> entities;
        std::size_t at = 0;
        const auto quoted = [&](std::string& out) {
            const auto open = text.find('"', at);
            if (open == std::string_view::npos)
                return false;
            const auto close = text.find('"', open + 1);
            if (close == std::string_view::npos)
                return false;
            out = std::string(text.substr(open + 1, close - open - 1));
            at = close + 1;
            return true;
        };
        while ((at = text.find('{', at)) != std::string_view::npos)
        {
            auto& entity = entities.emplace_back();
            ++at;
            for (;;)
            {
                const auto next = text.find_first_of("\"}", at);
                if (next == std::string_view::npos || text[next] == '}')
                {
                    at = next == std::string_view::npos ? text.size() : next + 1;
                    break;
                }
                std::string key, value;
                if (!quoted(key) || !quoted(value))
                    break;
                entity.emplace_back(std::move(key), std::move(value));
            }
        }
        return entities;
    }

    const std::string* FindKey(const Entity& entity, std::string_view key)
    {
        for (const auto& [k, v] : entity)
            if (k == key)
                return &v;
        return nullptr;
    }

    std::vector<glm::vec3> ChooseSpawnPoints(const glm::vec3& start, std::vector<glm::vec3> nodes)
    {
        const auto distance = [](const glm::vec3& a, const glm::vec3& b) {
            return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
        };
        std::ranges::stable_sort(nodes, {}, [&](const glm::vec3& node) { return distance(node, start); });
        std::vector<glm::vec3> points{start};
        for (const auto& node : nodes)
        {
            if (points.size() >= SPAWN_POINTS)
                break;
            const auto apart = std::ranges::all_of(points, [&](const glm::vec3& point) {
                return (point.x - node.x) * (point.x - node.x) + (point.y - node.y) * (point.y - node.y) >
                       SPAWN_SPACING * SPAWN_SPACING;
            });
            if (apart)
                points.push_back(node);
        }
        return points;
    }

    namespace
    {
        // groups nodes closer than AREA_LINK, on a grid of that size: each node looks at the 27 squares around it
        std::vector<std::vector<glm::vec3>> GroupNodes(std::span<const glm::vec3> nodes)
        {
            const auto key = [](const glm::vec3& node, int x, int y, int z) {
                return std::array{static_cast<int>(std::floor(node.x / AREA_LINK)) + x,
                                  static_cast<int>(std::floor(node.y / AREA_LINK)) + y,
                                  static_cast<int>(std::floor(node.z / AREA_LINK)) + z};
            };
            std::map<std::array<int, 3>, std::vector<std::uint32_t>> grid;
            for (std::uint32_t i = 0; i < nodes.size(); ++i)
                grid[key(nodes[i], 0, 0, 0)].push_back(i);
            std::vector<std::uint32_t> parent(nodes.size());
            std::iota(parent.begin(), parent.end(), 0u);
            const auto find = [&](std::uint32_t node) {
                while (parent[node] != node)
                    node = parent[node] = parent[parent[node]];
                return node;
            };
            for (std::uint32_t i = 0; i < nodes.size(); ++i)
                for (int x = -1; x <= 1; ++x)
                    for (int y = -1; y <= 1; ++y)
                        for (int z = -1; z <= 1; ++z)
                            if (const auto square = grid.find(key(nodes[i], x, y, z)); square != grid.end())
                                for (const auto other : square->second)
                                    if (glm::distance(nodes[i], nodes[other]) <= AREA_LINK)
                                        parent[find(i)] = find(other);
            std::map<std::uint32_t, std::vector<glm::vec3>> groups;
            for (std::uint32_t i = 0; i < nodes.size(); ++i)
                groups[find(i)].push_back(nodes[i]);
            std::vector<std::vector<glm::vec3>> result;
            for (auto& [root, group] : groups)
                result.push_back(std::move(group));
            return result;
        }

        std::pair<glm::vec3, glm::vec3> GetBounds(const std::vector<glm::vec3>& nodes)
        {
            auto mins = nodes.front(), maxs = nodes.front();
            for (const auto& node : nodes)
            {
                mins = glm::min(mins, node);
                maxs = glm::max(maxs, node);
            }
            return {mins, maxs};
        }

        // cuts a group wider than AREA_MOST_SPAN across its longest side, half of its nodes each way
        void SplitGroup(std::vector<glm::vec3> nodes, std::vector<std::vector<glm::vec3>>& parts)
        {
            const auto [mins, maxs] = GetBounds(nodes);
            const auto size = maxs - mins;
            const int axis = size.x >= size.y ? 0 : 1;
            if (size[axis] <= AREA_MOST_SPAN || nodes.size() < 2 * AREA_LEAST_NODES)
            {
                parts.push_back(std::move(nodes));
                return;
            }
            const auto middle = nodes.begin() + static_cast<std::ptrdiff_t>(nodes.size() / 2);
            std::ranges::nth_element(nodes, middle, [&](const glm::vec3& a, const glm::vec3& b) { return a[axis] < b[axis]; });
            std::vector<glm::vec3> first(nodes.begin(), middle), second(middle, nodes.end());
            SplitGroup(std::move(first), parts);
            SplitGroup(std::move(second), parts);
        }

        Area MakeArea(const std::vector<glm::vec3>& nodes)
        {
            Area area;
            std::tie(area.mins, area.maxs) = GetBounds(nodes);
            area.nodes = static_cast<std::uint32_t>(nodes.size());
            glm::vec3 middle(0.0f);
            for (const auto& node : nodes)
                middle += node / static_cast<float>(nodes.size());
            area.spot = *std::ranges::min_element(nodes, {}, [&](const glm::vec3& node) { return glm::distance(node, middle); });
            // towards the part's far end, the longest view of it from there
            const auto farthest = *std::ranges::max_element(nodes, {}, [&](const glm::vec3& node) { return glm::distance(node, area.spot); });
            area.yaw = glm::degrees(std::atan2(farthest.y - area.spot.y, farthest.x - area.spot.x));
            return area;
        }
    }  // namespace

    std::vector<Area> FindAreas(std::span<const glm::vec3> nodes, const glm::vec3& start)
    {
        std::vector<std::vector<glm::vec3>> parts;
        for (auto& group : GroupNodes(nodes))
            if (group.size() >= AREA_LEAST_NODES)
                SplitGroup(std::move(group), parts);
        std::vector<Area> left;
        for (const auto& part : parts)
            left.push_back(MakeArea(part));
        std::vector<Area> areas;
        auto from = start;
        while (!left.empty())
        {
            const auto nearest =
                std::ranges::min_element(left, {}, [&](const Area& area) { return glm::distance(area.spot, from); });
            from = nearest->spot;
            areas.push_back(*nearest);
            left.erase(nearest);
        }
        return areas;
    }

    namespace
    {
        // the network keeps whole units
        constexpr float ORIGIN_WITHIN = 1.5f;

        float WithinRange(float value)
        {
            return value - std::floor(value / NET_ORIGIN_RANGE) * NET_ORIGIN_RANGE;
        }

        bool SameInRange(float a, float b)
        {
            const auto apart = std::abs(WithinRange(a) - WithinRange(b));
            return std::min(apart, NET_ORIGIN_RANGE - apart) <= ORIGIN_WITHIN;
        }
    }

    LevelOrigins::LevelOrigins(const std::vector<glm::vec3>& origins)
    {
        placed.reserve(origins.size());
        for (const auto& origin : origins)
            placed.push_back({WithinRange(origin.x), origin});
        std::ranges::sort(placed, {}, &Placed::key);
    }

    std::optional<glm::vec3> LevelOrigins::Find(const glm::vec3& sent, const glm::vec3& viewer) const
    {
        std::optional<glm::vec3> found;
        const auto key = WithinRange(sent.x);
        // (the second and third for a position at the edge of the range)
        for (const auto from : {key, key - NET_ORIGIN_RANGE, key + NET_ORIGIN_RANGE})
        {
            auto at = std::ranges::lower_bound(placed, from - ORIGIN_WITHIN, {}, &Placed::key);
            for (; at != placed.end() && at->key <= from + ORIGIN_WITHIN; ++at)
                if (SameInRange(at->origin.y, sent.y) && SameInRange(at->origin.z, sent.z) &&
                    (!found || glm::distance(at->origin, viewer) < glm::distance(*found, viewer)))
                    found = at->origin;
        }
        return found;
    }

    glm::vec3 UnwrapOrigin(const glm::vec3& origin, const glm::vec3& viewer, const LevelOrigins& placed)
    {
        // (what's sent is in whole units: only the ranges are added, to it)
        const auto toward = placed.Find(origin, viewer).value_or(viewer);
        return origin + glm::round((toward - origin) / NET_ORIGIN_RANGE) * NET_ORIGIN_RANGE;
    }

    bool IsFlickeringLight(const Entity& entity)
    {
        const auto classname = FindKey(entity, "classname");
        const auto name = FindKey(entity, "targetname");
        return classname && *classname == "light" && name && *name == "fire_flicker";
    }

    namespace
    {
        // (a door, a gate, a shutter or a hatch opens; a blocker or a barricade doesn't, and stays: most are the
        // player's clips for a moment of the mission, the rest rubble, crates, sandbags or a plane's wreck)
        constexpr std::array DOOR_WORDS{"door", "gate", "shutter", "hatch"};
        // (creek_1's thatch roof has a hatch in its name)
        constexpr std::array NOT_DOOR_WORDS{"thatch"};

        bool IsDoorWord(std::string_view text)
        {
            const auto has = [&](const char* word) { return text.find(word) != text.npos; };
            return std::ranges::any_of(DOOR_WORDS, has) && std::ranges::none_of(NOT_DOOR_WORDS, has);
        }

        bool IsScripted(const Entity& entity)
        {
            const auto classname = FindKey(entity, "classname");
            const auto name = FindKey(entity, "targetname");
            return classname && name && *name != "destructible" &&
                   (*classname == "script_brushmodel" || *classname == "script_model");
        }

        bool IsBrush(const Entity& entity)
        {
            return *FindKey(entity, "classname") == "script_brushmodel";
        }

        // a level's doors by name: what a Zombies map's door and debris triggers open, and on a campaign level the
        // models that are a door by their own name and the brush models named like one (a name made by the editor, as
        // a prefab's, says nothing)
        struct DoorNames
        {
            std::set<std::string, std::less<>> opened, doors;
        };

        DoorNames FindDoorNames(const std::vector<Entity>& entities)
        {
            DoorNames names;
            for (const auto& entity : entities)
            {
                const auto classname = FindKey(entity, "classname");
                const auto name = FindKey(entity, "targetname");
                const auto target = FindKey(entity, "target");
                if (classname && classname->starts_with("trigger_") && name && target &&
                    (*name == "zombie_door" || *name == "zombie_debris"))
                    names.opened.insert(*target);
            }
            for (const auto& entity : entities)
            {
                if (!IsScripted(entity))
                    continue;
                const auto& name = *FindKey(entity, "targetname");
                const auto model = FindKey(entity, "model");
                if (names.opened.contains(name) || (IsBrush(entity) ? IsDoorWord(name) : model && IsDoorWord(*model)))
                    names.doors.insert(name);
            }
            return names;
        }

        // Whether an entity is a piece of a door: all a Zombies trigger opens; a model that is a door by its own name;
        // a brush model, which has none, of a door's name, so a prefab's door takes its clip along. What else a door's
        // name has stays: vorkuta's wall switches (door_button_before), the 24 shotguns on the floor of
        // sergei_weapon_door_dmg, Ascension's lander caps.
        bool IsDoorPiece(const Entity& entity, const DoorNames& names)
        {
            if (!IsScripted(entity))
                return false;
            const auto& name = *FindKey(entity, "targetname");
            const auto model = FindKey(entity, "model");
            return names.opened.contains(name) ||
                   (IsBrush(entity) ? names.doors.contains(name) : model && IsDoorWord(*model));
        }

        std::vector<Door> ListDoors(const std::vector<Entity>& entities, const DoorNames& names)
        {
            std::vector<Door> doors;
            for (const auto& entity : entities)
            {
                if (!IsDoorPiece(entity, names))
                    continue;
                const auto& name = *FindKey(entity, "targetname");
                auto door = std::ranges::find(doors, name, &Door::name);
                if (door == doors.end())
                {
                    door = doors.insert(doors.end(), Door{.name = name, .zombies = names.opened.contains(name)});
                    if (const auto origin = ReadVector(FindKey(entity, "origin")))
                        door->at = *origin;
                }
                ++door->pieces;
            }
            return doors;
        }
    }  // namespace

    std::vector<Door> FindDoors(std::string_view text)
    {
        const auto entities = ParseEntities(text);
        return ListDoors(entities, FindDoorNames(entities));
    }

    bool IsParkedVehicle(const Entity& entity)
    {
        if (FindKey(entity, "script_vehiclespawngroup") || FindKey(entity, "target"))
            return false;
        if (const auto flags = FindKey(entity, "spawnflags"); flags && (std::atoi(flags->c_str()) & VEHICLE_SPAWNER))
            return false;
        const auto type = FindKey(entity, "vehicletype");
        return !type || !std::ranges::any_of(NOT_PARKED_TYPES, [&](std::string_view prefix) {
            return type->starts_with(prefix);
        });
    }

    Rewrite RewriteEntities(std::string_view text, const std::function<bool(const std::string&)>& hasModel,
                            const RewriteOptions& options)
    {
        Rewrite rewrite;
        const auto entities = ParseEntities(text);
        const auto doorNames = FindDoorNames(entities);
        rewrite.doors = ListDoors(entities, doorNames);
        const auto isOpen = [&](const std::string& name) {
            return options.doors == Doors::AllOpen ||
                   (options.doors == Doors::Open && !options.keptDoors.contains(name));
        };
        for (auto& door : rewrite.doors)
            door.open = isOpen(door.name);
        std::vector<glm::vec3> nodes;
        std::optional<glm::vec3> start;
        for (const auto& entity : entities)
        {
            const auto classname = FindKey(entity, "classname");
            const auto origin = ReadVector(FindKey(entity, "origin"));
            if (classname && origin && classname->starts_with("node_"))
                nodes.push_back(*origin);
            if (const auto animscript = FindKey(entity, "animscript"))
                rewrite.traversals.insert(*animscript);
            if (classname && origin && *classname == "info_player_start" && !start)
            {
                start = origin;
                if (const auto angles = ReadVector(FindKey(entity, "angles")))
                    rewrite.startYaw = angles->y;
            }
            const auto model = FindKey(entity, "model");
            const bool missingModel = hasModel && model && !model->starts_with('*') && !hasModel(*model);
            // (the world is the first entity, or the game stops: "SP_worldspawn: The first entity isn't 'worldspawn'";
            // Five's names a model, p_rus_mop, which only that key loses)
            const bool world = classname && *classname == "worldspawn";
            const bool vehicle = classname && *classname == "script_vehicle";
            if (vehicle)
                ++(IsParkedVehicle(entity) && !missingModel ? rewrite.vehiclesKept : rewrite.vehiclesLeftOut);
            // (a mounted gun with nobody to fire it is its picture: 21 on the levels, 4 M60s on hue_city)
            const bool turret = classname && *classname == "misc_turret";
            const bool asModel = vehicle || turret;
            const bool openDoor = IsDoorPiece(entity, doorNames) && isOpen(*FindKey(entity, "targetname"));
            if (openDoor)
                ++rewrite.doorPiecesLeftOut;
            if (!classname || !KEPT_CLASSES.contains(*classname) || (missingModel && !world) ||
                (vehicle && !IsParkedVehicle(entity)) || openDoor)
            {
                ++rewrite.dropped;
                continue;
            }
            ++rewrite.kept;
            const bool steady = options.steadyLights && IsFlickeringLight(entity);
            if (steady)
                ++rewrite.lightsMadeSteady;
            rewrite.entities += "{\n";
            for (const auto& [key, value] : entity)
            {
                if (DROPPED_KEYS.contains(key) || (asModel && DROPPED_VEHICLE_KEYS.contains(key)) ||
                    (world && missingModel && key == "model"))
                    continue;
                rewrite.entities += std::format("\"{}\" \"{}\"\n", key,
                                                asModel && key == "classname" ? "script_model"
                                                : steady && key == "targetname" ? "light_solid"
                                                                                : value);
            }
            rewrite.entities += "}\n";
            if (origin)
                rewrite.origins.push_back(*origin);
        }

        rewrite.start = start ? *start : !nodes.empty() ? nodes.front() : glm::vec3(0.0f);
        rewrite.areas = FindAreas(nodes, rewrite.start);
        for (const auto& node : nodes)
            rewrite.nodes.emplace_back(node.x, node.y);
        rewrite.spawnPoints = ChooseSpawnPoints(rewrite.start, std::move(nodes));
        const auto yaw = FormatNumber(rewrite.startYaw);
        for (const auto classname : SPAWN_CLASSES)
        {
            for (const auto& point : rewrite.spawnPoints)
            {
                rewrite.entities += std::format("{{\n\"classname\" \"{}\"\n\"origin\" \"{}\"\n\"angles\" \"0 {} 0\"\n}}\n",
                                                classname, FormatVector(point), yaw);
                ++rewrite.spawns;
            }
        }
        rewrite.entities += std::format(
            "{{\n\"classname\" \"mp_global_intermission\"\n\"origin\" \"{}\"\n\"angles\" \"10 {} 0\"\n}}\n",
            FormatVector(rewrite.start + glm::vec3(0.0f, 0.0f, 80.0f)), yaw);
        return rewrite;
    }

    std::string GetMapScriptName(std::string_view level)
    {
        return std::format("maps/mp/{}.gsc", level);
    }

    std::string GetClientScriptName(std::string_view level)
    {
        return std::format("clientscripts/mp/{}.csc", level);
    }

    std::string MakeMapScript(const Level& level, const std::vector<std::string>& playerModels)
    {
        auto text = std::format(
            "main()\r\n{{\r\n\tmaps\\mp\\_load::main();\r\n\tmaps\\mp\\gametypes\\_teamset_{}::level_init();\r\n"
            "\tmaps\\mp\\gametypes\\_spawning::level_use_unified_spawning(true);\r\n"
            "\tsetdvar(\"compassmaxrange\", \"2100\");\r\n",
            GetTeamsetName(level.teamset));
        if (!level.fog.empty())
            text += std::format("\tsetVolFog({});\r\n", level.fog);
        if (!level.vision.empty())
            text += std::format("\tVisionSetNaked(\"{}\", 0);\r\n", level.vision);
        // Multiplayer's _teams::setPlayerModels takes the players' look from mp/mapsTable.csv by the map's name, where a
        // campaign or Zombies map isn't, and falls back to the jungle marines: it precaches their factions' models while
        // the players are dressed in the level's teamset's, which nothing precached, so they had no body (and the arms
        // of the default model). Its teamset's player models are precached here, in the level's first frame as the
        // game asks; the jungle marines' levels had theirs already.
        if (level.teamset != Teamset::JungleMarines)
            for (const auto& model : playerModels)
                text += std::format("\tPrecacheModel(\"{}\");\r\n", model);
        return text + "}\r\n";
    }

    std::string MakeClientScript(const Level& level)
    {
        return std::format(
            "#include clientscripts\\mp\\_utility;\r\n\r\nmain()\r\n{{\r\n"
            "\tclientscripts\\mp\\_teamset_{}::level_init();\r\n\tclientscripts\\mp\\_load::main();\r\n"
            "\tthread clientscripts\\mp\\_audio::audio_init(0);\r\n\tthread waitforclient(0);\r\n}}\r\n",
            GetTeamsetName(level.teamset));
    }

    std::string GetTraversalScriptName(std::string_view animscript)
    {
        // (the game makes a script's file name in 64 characters with the terminator, so a longer one is asked for cut
        // short: Call of the Dead's maps/mp/animscripts/traverse/zombie_zipline_traverse_lighthouse, without .gsc)
        constexpr std::size_t MAX_QPATH = 64;
        auto name = std::format("maps/mp/animscripts/traverse/{}.gsc", animscript);
        if (name.size() >= MAX_QPATH)
            name.resize(MAX_QPATH - 1);
        return name;
    }

    std::string_view GetEmptyScript()
    {
        return "main()\r\n{\r\n}\r\n";
    }

    std::vector<std::uint8_t> PackScript(std::string_view text)
    {
        std::vector<std::uint8_t> plain(text.begin(), text.end());
        plain.push_back(0);
        const auto packed = Deflate::Zlib(plain);
        std::vector<std::uint8_t> script(8);
        const auto sizes = std::array{static_cast<std::uint32_t>(plain.size()), static_cast<std::uint32_t>(packed.size())};
        std::memcpy(script.data(), sizes.data(), 8);
        script.insert(script.end(), packed.begin(), packed.end());
        return script;
    }

    ScriptFile MakeScriptFile(std::string_view name, std::string_view text)
    {
        ScriptFile file;
        if (name.ends_with(".gsc") || name.ends_with(".csc"))
        {
            file.buffer = PackScript(text);
            file.length = static_cast<std::int32_t>(file.buffer.size());
            return file;
        }
        file.buffer.assign(text.begin(), text.end());
        file.buffer.push_back(0);
        file.length = static_cast<std::int32_t>(text.size());
        return file;
    }

    std::optional<std::string> UnpackScript(std::span<const std::uint8_t> file)
    {
        if (file.size() < 8)
            return std::nullopt;
        std::array<std::uint32_t, 2> sizes{};
        std::memcpy(sizes.data(), file.data(), 8);
        if (sizes[0] == 0 || sizes[1] > file.size() - 8)
            return std::nullopt;
        const auto plain = Inflate::Zlib(file.subspan(8, sizes[1]), sizes[0]);
        if (!plain || plain->empty())
            return std::nullopt;
        return std::string(plain->begin(), plain->end() - 1);
    }

    bool IsFixedClientScript(std::string_view name)
    {
        return name == "clientscripts/mp/mp_russianbase.csc";
    }

    std::optional<std::string> FixClientScript(std::string_view source)
    {
        // each once in the script, a line added before or after it
        struct Change
        {
            std::string_view find;
            std::string_view add;
            bool after;
        };
        const std::array<Change, 4> CHANGES{{
            {"originalRatio = ( moveTime / 80 );", "level endon( \"iris_rewind\" );\r\n\t", false},
            {"trainEngine SetModel( engineType );", "\r\n\ttrainEngine thread deleteOnRewind();", true},
            {"trainCar SetModel( carType );", "\r\n\ttrainCar thread deleteOnRewind();", true},
            // (a horn's wait outlasts its car)
            {"self endon( \"movedone\" );", "\r\n\tself endon( \"entityshutdown\" );", true},
        }};
        std::string text(source);
        for (const auto& change : CHANGES)
        {
            const auto at = text.find(change.find);
            if (at == std::string::npos || text.find(change.find, at + 1) != std::string::npos)
                return std::nullopt;
            text.insert(change.after ? at + change.find.size() : at, change.add);
        }
        text +=
            "\r\ndeleteOnRewind()\r\n{\r\n\tself endon( \"movedone\" );\r\n\tself endon( \"entityshutdown\" );\r\n"
            "\tlevel waittill( \"iris_rewind\" );\r\n\tself Delete();\r\n}\r\n";
        return text;
    }

    std::string_view GetEffectKindName(std::string_view definition)
    {
        const auto slash = definition.find_last_of("/\\");
        return slash == std::string_view::npos ? definition : definition.substr(slash + 1);
    }

    std::vector<EffectKindRow> ListEffectKinds(const Level& level)
    {
        std::vector<EffectKindRow> rows;
        for (std::uint32_t kind = 0; kind < level.effectKinds.size(); ++kind)
            rows.push_back({kind, GetEffectKindName(level.effectKinds[kind]), 0});
        for (const auto& effect : level.effects)
            ++rows[effect.kind].placed;
        std::erase_if(rows, [](const EffectKindRow& row) { return row.placed == 0; });
        std::ranges::stable_sort(rows, {}, &EffectKindRow::name);
        return rows;
    }

    std::int32_t GetEffectBegin(const MapEffect& effect, std::int32_t start)
    {
        // a oneshot with a delay is waited for, then played with that delay (oneshotfxthread, create_triggerfx); a loop
        // plays at once with it (loopfx)
        const auto delay = effect.delay > 0.0f && !effect.loop ? effect.delay * 2.0f : effect.delay;
        return start + static_cast<std::int32_t>(std::lround(delay * 1000.0f));
    }

    bool PlaysAgain(const MapEffect& effect, std::int32_t now, std::int32_t spawnedAt, bool playing)
    {
        if (!effect.loop || playing)
            return false;
        return now - spawnedAt >= static_cast<std::int32_t>(std::lround(std::max(effect.delay, 0.0f) * 1000.0f));
    }

    Budget MeasureBudget(const Level& level, std::uint32_t freeImages, std::uint32_t freeMaterials,
                         std::uint32_t freeModels)
    {
        Budget budget;
        budget.images = static_cast<std::int64_t>(freeImages) - level.newImages;
        budget.materials = static_cast<std::int64_t>(freeMaterials) - level.newMaterials;
        budget.models = static_cast<std::int64_t>(freeModels) - level.newModels;
        budget.fits = budget.images >= 0 && budget.materials >= 0 && budget.models >= 0;
        if (budget.fits)
            budget.characters = static_cast<std::uint32_t>(
                std::min(budget.images / IMAGES_A_CHARACTER, budget.models / MODELS_A_CHARACTER));
        return budget;
    }
}  // namespace IWXMVM::Components::CampaignMapLogic
