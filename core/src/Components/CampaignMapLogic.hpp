#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace IWXMVM::Components::CampaignMapLogic
{
    // Campaign levels and Zombies maps loaded with devmap as multiplayer maps, canvases for actors:
    // the game loads the level's zone and only its world is registered (the assets
    // OAT's Linker links of it, listed by tools/t5-maps into core/resources/campaign-maps.json), under multiplayer's
    // types and names, with its entities rewritten for multiplayer, stand-in scripts, and the players' kit (a zone per
    // teamset, built at setup). No game state; tested in tests/actors-tests.

    // The players' look on a level: the multiplayer teamset of the level's setting
    enum class Teamset
    {
        UrbanSpecOps,
        JungleMarines,
        WinterSpecOps,
        Cubans,
    };
    // as the game's scripts name it ("urbanspecops": maps/mp/gametypes/_teamset_urbanspecops.gsc)
    std::string_view GetTeamsetName(Teamset teamset);
    std::optional<Teamset> FindTeamset(std::string_view name);
    // the zone of the players' kit for it (built at setup from core/resources/campaign-kits.json, see SetupLogic: the
    // body, head and dog scripts, the teamset's player and arm models, the RC-XD, bomb and HQ crate and their effects,
    // out of a multiplayer map of that teamset)
    std::string GetKitZone(Teamset teamset);
    inline constexpr std::string_view KIT_ZONE_PREFIX = "t5m_kit_";

    // An ambient effect the level's createfx places: fire, smoke, dust, lights. As the
    // campaign's client script plays them (clientscripts\_fx.csc): a oneshot once as the map starts, delay seconds into
    // its life (-15 for most: already burning, a positive delay waited and then given again), a loop at once and again
    // each delay seconds once the play before has ended
    struct MapEffect
    {
        std::uint32_t kind = 0;  // its definition, an index into Level::effectKinds
        glm::vec3 origin{0.0f};
        glm::vec3 angles{0.0f};  // the script's: it fires along their forward, their up its up
        float delay = 0.0f;      // seconds
        bool loop = false;
    };

    // What the level was made for: a mission of the campaign, or a Zombies map, which loads
    // the same way
    enum class Mode
    {
        Campaign,
        Zombies,
    };
    // as the log and the harness name such a level: "campaign level", "Zombies map"
    std::string_view GetModeName(Mode mode);

    // A level of campaign-maps.json
    struct Level
    {
        std::string name;  // its zone, "cuba"
        std::string title;
        Mode mode = Mode::Campaign;
        Teamset teamset = Teamset::UrbanSpecOps;
        // the zones single player loads before it for its mode that its world takes assets of (common_zombie for most
        // Zombies maps: their effects' materials and technique sets), loaded with it the same way, world-only
        std::vector<std::string> zones;
        std::string fog;     // the arguments of its art script's first setVolFog, empty for none
        std::string vision;  // its first VisionSetNaked's, empty for none
        // the assets its world is made of (gfxworld and the rest aside), by the type names of OAT's lists: image,
        // material, techniqueset, xmodel, physpreset
        std::map<std::string, std::set<std::string>> world;
        // what of them multiplayer's base zones don't have (the room the world takes), measured by the tool
        std::uint32_t newImages = 0, newMaterials = 0, newModels = 0, newEffects = 0;
        // its ambient effects (not the exploders its script sets off), and the definitions they play, sorted
        std::vector<std::string> effectKinds;
        std::vector<MapEffect> effects;
        // its row in the game's Change Map menu (Components::MapMenuLogic): its place in its
        // tab, the language keys of its name in capitals and of its description, and its picture, an image of a single
        // player zone as "frontend/cuba" (empty for none)
        struct MenuEntry
        {
            std::uint32_t order = 0;
            std::string name, description, picture;
        };
        std::optional<MenuEntry> menu;
        // its doors that open into nothing, kept closed: a door with no room built behind it, a lander pad's floor over
        // the void (found by looking at each door open in the game, tools/t5-maps/closed-doors.json)
        std::set<std::string, std::less<>> closedDoors;
    };

    struct Catalog
    {
        std::vector<Level> levels;
        // the players' bodies, heads and arms of each teamset's kit (its factions on both sides)
        std::map<Teamset, std::vector<std::string>> playerModels;
        // the texts of the menu's language keys that multiplayer's language files lack (single player's, ours), UTF-8
        std::map<std::string, std::string, std::less<>> strings;

        const Level* FindLevel(std::string_view name) const;
    };
    // throws std::runtime_error for a catalog it can't read
    Catalog ReadCatalog(std::string_view json);

    // Why a film of the level won't play when the game folder doesn't have its zone (a DLC map it lacks), in words for
    // the Demo tab's notice and the console
    std::string GetMissingMapRefusal(const Level& level);

    // The level a zone the game loads belongs to: the level's own or its language twin's (en_cuba), none for others
    const Level* FindLevelOfZone(const Catalog& catalog, std::string_view zone);
    // Whether a zone loads with the level without being its own: its Level::zones, their language and patch twins
    // (en_common_zombie, common_zombie_patch) and its own patch (zombie_pentagon_patch), which the game loads with a
    // zone of that name when it has one
    bool IsLoadedWith(const Level& level, std::string_view zone);
    // whether it loads so with any level
    bool IsLoadedWithAny(const Catalog& catalog, std::string_view zone);

    // the level's world, as single player names it (maps/cuba.d3dbsp) and as it is registered for multiplayer
    // (maps/mp/cuba.d3dbsp: multiplayer's scripts find a map's world by that name)
    std::string GetWorldName(std::string_view level);
    std::string GetMultiplayerWorldName(std::string_view level);

    // The map's entities ({ "key" "value" ... } blocks), as the game's parser reads them
    using Entity = std::vector<std::pair<std::string, std::string>>;
    std::vector<Entity> ParseEntities(std::string_view text);
    const std::string* FindKey(const Entity& entity, std::string_view key);

    // How far apart spawn points stand at least, and how many there are of each game type's class
    inline constexpr float SPAWN_SPACING = 64.0f;
    inline constexpr std::size_t SPAWN_POINTS = 16;
    // Where players spawn: the level's start, then the path nodes nearest it that stand SPAWN_SPACING apart from the
    // points before them (across, not up), up to SPAWN_POINTS
    std::vector<glm::vec3> ChooseSpawnPoints(const glm::vec3& start, std::vector<glm::vec3> nodes);

    // A part of a level to go to: a campaign level is parts far apart, drawn only once the
    // camera is in them, and the path nodes are where a person can stand. spot is a node near the part's middle, where
    // to stand, facing yaw towards the rest of the part.
    struct Area
    {
        glm::vec3 spot{0.0f};
        float yaw = 0.0f;
        glm::vec3 mins{0.0f}, maxs{0.0f};
        std::uint32_t nodes = 0;
    };
    // nodes closer than this are of one group
    inline constexpr float AREA_LINK = 1200.0f;
    // a group of fewer is a corner of something, no place to go
    inline constexpr std::uint32_t AREA_LEAST_NODES = 12;
    // a group wider than this is cut in two, across its longest side, until none is: a level one long walk has no gaps
    inline constexpr float AREA_MOST_SPAN = 6000.0f;
    // The level's parts in the order a walk from its start meets them: the one nearest the start, the nearest to that...
    std::vector<Area> FindAreas(std::span<const glm::vec3> nodes, const glm::vec3& start);

    // A door of a level, left out so it stands open (there's no choosing, every door is open, always): on a
    // Zombies map what a door's or debris pile's trigger names, which is the door, its other half and
    // its clip; on a campaign level, where every level has names of its own, the models that are a door, a gate, a
    // shutter or a hatch by their own name, and the brush models of such a name, the doors' clips. A door is one name.
    struct Door
    {
        std::string name;       // the targetname of its pieces
        bool zombies = false;   // what a Zombies trigger opens, rather than found by its name
        std::uint32_t pieces = 0;
        glm::vec3 at{0.0f};     // where the first piece is
        bool open = true;       // left out by the rewrite
    };
    std::vector<Door> FindDoors(std::string_view text);

    // Which doors a level's rewrite leaves out
    enum class Doors
    {
        Open,     // every one but those that open into nothing (RewriteOptions::keptDoors)
        AllOpen,  // those too, for the harness to look at them
        Closed,   // none, as the mission starts, for a test to compare
    };
    // What a level's rewrite does otherwise, for the harness to compare: by default the lights flicker as the
    // mission's do and every door is open, with nothing to choose
    struct RewriteOptions
    {
        bool steadyLights = false;  // the lights named fire_flicker burn steadily, see IsFlickeringLight
        Doors doors = Doors::Open;
        std::set<std::string, std::less<>> keptDoors;  // the level's Level::closedDoors
    };
    // Lights multiplayer's and the campaign's _lights.csc run the same way, intensity going up and down for ever
    bool IsFlickeringLight(const Entity& entity);

    struct Rewrite
    {
        std::string entities;  // the entity string multiplayer gets
        std::uint32_t kept = 0, dropped = 0, spawns = 0;
        std::uint32_t vehiclesKept = 0, vehiclesLeftOut = 0;  // of those counted above (see IsParkedVehicle)
        std::uint32_t lightsMadeSteady = 0;   // by the options
        std::uint32_t doorPiecesLeftOut = 0;  // the doors' pieces, left out so they stand open
        std::vector<Door> doors;              // all it found, open or not
        std::vector<glm::vec3> origins;                             // where what it kept stands
        std::set<std::string> traversals;  // the animscripts its path nodes name (see GetTraversalScript)
        glm::vec3 start{0.0f};             // where the level starts (its info_player_start, else a path node)
        float startYaw = 0.0f;
        std::vector<glm::vec3> spawnPoints;
        std::vector<Area> areas;
        std::vector<glm::vec2> nodes;  // its path nodes from above, for a map of it
    };
    // The game sends an entity's position in 16 bits around a point of the map, so 65536 units one way or the other read
    // the same. Multiplayer's maps are smaller than that; a campaign level isn't, and what lies past the end of that range
    // came out a whole range away: 210 of the 261 entities the client got on wmd, its desks among them. What a level
    // placed is found by that position a whole number of ranges away, nearest the player if two fit; what has moved goes by
    // the player alone, which is right within half a range (and a level can show things further off, river's 90 of 210).
    inline constexpr float NET_ORIGIN_RANGE = 65536.0f;
    // the positions a level places things at, to look one up by what the network sends
    class LevelOrigins
    {
    public:
        explicit LevelOrigins(const std::vector<glm::vec3>& origins = {});
        std::optional<glm::vec3> Find(const glm::vec3& sent, const glm::vec3& viewer) const;

    private:
        struct Placed
        {
            float key;  // x, within the range
            glm::vec3 origin;
        };
        std::vector<Placed> placed;  // by key
    };
    glm::vec3 UnwrapOrigin(const glm::vec3& origin, const glm::vec3& viewer, const LevelOrigins& placed = LevelOrigins{});

    // Whether a level's vehicle stands where it stands from the start. Most are spawners a mission's script brings in
    // later, standing at the start of their path, often high in the air: those of a spawn group, on a path or with
    // the spawner flag, no one key on all levels (cuba flags all 84, wmd none). An aircraft or a marker with none of
    // those keys is a piece of a scene too: hue_city's opening helicopters hang at 9,000 units.
    bool IsParkedVehicle(const Entity& entity);

    // The level's entities as multiplayer can spawn them: the world, models, brush models, origins, structs and
    // lights; parked vehicles as plain models, without what made them destructible or drove them;
    // no AI, path and vehicle nodes, triggers, ropes or weapons, nor models multiplayer won't have (hasModel, when
    // given, says which it will). Then spawn points of every game type at the points of ChooseSpawnPoints, facing as
    // the level's start does, and the intermission's camera above the start.
    Rewrite RewriteEntities(std::string_view text, const std::function<bool(const std::string&)>& hasModel = {},
                            const RewriteOptions& options = {});

    // The stand-in scripts: maps/mp/<level>.gsc (multiplayer's map setup with the level's teamset, unified spawning,
    // and its first fog and vision), clientscripts/mp/<level>.csc, and an empty one for each traversal the
    // path nodes name (the game loads maps/mp/animscripts/traverse/<animscript>.gsc for each and stops without one)
    std::string GetMapScriptName(std::string_view level);
    std::string GetClientScriptName(std::string_view level);
    // (playerModels: the kit's of the level's teamset, Catalog::playerModels, which it precaches)
    std::string MakeMapScript(const Level& level, const std::vector<std::string>& playerModels = {});
    std::string MakeClientScript(const Level& level);
    std::string GetTraversalScriptName(std::string_view animscript);
    std::string_view GetEmptyScript();

    // A script as zones have them, for a rawfile: the size of the text with its terminator, the size of the zlib
    // data, the zlib data
    std::vector<std::uint8_t> PackScript(std::string_view text);
    // A script's rawfile as the game reads one of that name: packed (PackScript) for a .gsc or .csc, else the text and
    // its terminator, as the game reads a name cut short of its extension (see GetTraversalScriptName); the length is
    // what the rawfile says (the buffer's for a packed one, the text's for a plain one)
    struct ScriptFile
    {
        std::vector<std::uint8_t> buffer;
        std::int32_t length = 0;
    };
    ScriptFile MakeScriptFile(std::string_view name, std::string_view text);
    // A script's text out of a rawfile that PackScript's form holds; none when it isn't that
    std::optional<std::string> UnpackScript(std::span<const std::uint8_t> file);

    // What a rewind tells the client scripts (the client script callback level_notify, as the game's own clientNotify)
    inline constexpr const char* CLIENT_REWIND_NOTIFY = "iris_rewind";
    // Multiplayer WMD's train is 41 cars a client script spawns when the server says play_train and deletes when each
    // ends its move, so a rewind past that moment, which plays the message again, started a whole second train with
    // the first still there. This is that script, the train going with a rewind: the text
    // with the changes, none for another script, or if this one isn't the text expected
    bool IsFixedClientScript(std::string_view name);
    std::optional<std::string> FixClientScript(std::string_view text);

    // The map effects as the Look tab lists them: a row for each definition, named after its last part
    // ("fx_fire_line_lg" of "env/fire/fx_fire_line_lg"), with how many the level places, by name
    struct EffectKindRow
    {
        std::uint32_t kind = 0;
        std::string_view name;
        std::uint32_t placed = 0;
    };
    std::string_view GetEffectKindName(std::string_view definition);
    std::vector<EffectKindRow> ListEffectKinds(const Level& level);

    // When a map effect starts in the client's time (ms), for the map's start: its first play's begin, which may be in
    // the past (an effect already playing) or later
    std::int32_t GetEffectBegin(const MapEffect& effect, std::int32_t start);
    // Whether a loop plays again now: its delay since its play before was spawned has passed and that play has ended
    // (the client script's loopfx); not at a time before that spawn (the demo rewound: the binding counts from then)
    bool PlaysAgain(const MapEffect& effect, std::int32_t now, std::int32_t spawnedAt, bool playing);

    // The room a level leaves for actors (the free slots once its world is in): what fits of the characters, at the
    // images and models a character takes most often (character zones: about 25 images and 6 models)
    inline constexpr std::uint32_t IMAGES_A_CHARACTER = 25;
    inline constexpr std::uint32_t MODELS_A_CHARACTER = 6;
    struct Budget
    {
        std::int64_t images = 0, materials = 0, models = 0;  // left, below 0 when the world doesn't fit
        std::uint32_t characters = 0;
        bool fits = false;  // the world fits (else the map isn't loaded)
    };
    Budget MeasureBudget(const Level& level, std::uint32_t freeImages, std::uint32_t freeMaterials,
                         std::uint32_t freeModels);
}  // namespace IWXMVM::Components::CampaignMapLogic
