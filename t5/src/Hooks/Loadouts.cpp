#include "StdInclude.hpp"
#include "Loadouts.hpp"

#include <set>

#include "Mod.hpp"
#include "Events.hpp"
#include "Components/Actors.hpp"
#include "Components/Loadouts.hpp"
#include "Components/WeaponCatalog.hpp"
#include "Utilities/HookManager.hpp"
#include "../Actors.hpp"
#include "../Addresses.hpp"
#include "../CampaignMaps.hpp"
#include "../ContentPacks.hpp"
#include "../Functions.hpp"
#include "../LiveActors.hpp"
#include "../Structures.hpp"
#include "../WeaponRig.hpp"

namespace IWXMVM::T5::Hooks::Loadouts
{
    using namespace Structures;

    // Players are drawn from their entity state and their clientInfo_t. Every frame, before the game updates their
    // model, the weapon and camo in the entity state are replaced, like the death animations replace corpse states
    // (see PlayerAnimation.cpp), and put back afterwards. The clientInfo_t gets the chosen body and head; a changed
    // config string (or a rewind) puts the demo's back, so they are checked every frame too.

    // The guns of multiplayer, by category. Anything else a player holds (equipment, killstreaks) keeps its look.
    const std::vector<std::pair<std::string_view, std::vector<std::string_view>>> CATEGORIES = {
        {"Submachine Guns", {"mp5k", "skorpion", "mac11", "ak74u", "uzi", "pm63", "mpl", "spectre", "kiparis"}},
        {"Assault Rifles", {"m16", "enfield", "m14", "famas", "galil", "aug", "fnfal", "ak47", "commando", "g11"}},
        {"Shotguns", {"rottweil72", "ithaca", "spas", "hs10"}},
        {"Light Machine Guns", {"hk21", "rpk", "m60", "stoner63"}},
        {"Sniper Rifles", {"dragunov", "wa2000", "l96a1", "psg1"}},
        {"Pistols", {"asp", "m1911", "makarov", "python", "cz75"}},
        {"Launchers", {"m72_law", "rpg", "strela", "china_lake"}},
        {"Specials", {"crossbow_explosive", "knife_ballistic"}},
        {"Killstreaks", {"minigun", "m220_tow", "m202_flash"}},
    };

    const std::set<std::string_view> SECONDARY_CATEGORIES = {"Pistols", "Launchers", "Specials"};

    // what actors can hold but a player's weapon rule doesn't list: a player holding one keeps it, as any other
    // equipment, and a rule's "any weapon" doesn't turn them into one
    const std::set<std::string_view> ACTORS_ONLY_CATEGORIES = {"Killstreaks"};

    // The parts of variant names, listed sights first. Dual wield guns are weapons of their own, named <gun>dw_mp,
    // which are listed as the gun with the dw attachment.
    const std::vector<std::pair<std::string_view, std::string_view>> ATTACHMENTS = {
        {"reflex", "Reflex Sight"},  {"elbit", "Red Dot Sight"},    {"acog", "ACOG Sight"},
        {"ir", "Infrared Scope"},    {"lps", "Low Power Scope"},    {"vzoom", "Variable Zoom"},
        {"upgradesight", "Upgraded Iron Sights"},                   {"silencer", "Suppressor"},
        {"extclip", "Extended Mags"}, {"dualclip", "Dual Mag"},      {"speed", "Speed Reloader"},
        {"grip", "Grip"},            {"rf", "Rapid Fire"},          {"auto", "Full Auto Upgrade"},
        {"snub", "Snub Nose"},       {"gl", "Grenade Launcher"},    {"mk", "Masterkey"},
        {"ft", "Flamethrower"},      {"dw", "Dual Wield"},
    };

    // the camos of mp/weaponoptions.csv, by number, with a color close to their look
    const std::vector<Types::LoadoutCatalog::Camo> CAMOS = {
        {"None", 0x3A3A3A},     {"Dusty", 0xA89A80},   {"Ice", 0xC8DCE6},     {"Red", 0x8E2A22},
        {"Olive", 0x5A6236},    {"Nevada", 0xB49A6E},  {"Sahara", 0xC8A872},  {"ERDL", 0x4E5A34},
        {"Tiger", 0x55502E},    {"Berlin", 0x7A7A78},  {"Warsaw", 0x5E6670},  {"Siberia", 0xA8ACB0},
        {"Yukon", 0xD8DCDC},    {"Woodland", 0x4A4A2C}, {"Flora", 0x6A7040},  {"Gold", 0xD4AF37},
    };

    // the names of the weapon variants (by index) the catalog lists, empty for the others
    std::array<std::string, MAX_WEAPON_VARIANTS> listedVariants;
    std::unordered_map<std::string, std::uint16_t> variantIndices;
    std::shared_ptr<const Types::LoadoutCatalog> builtCatalog;

    std::string_view GetVariantName(std::uint16_t index)
    {
        if (index == 0 || index >= MAX_WEAPON_VARIANTS)
            return {};
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        const auto variant = variants[index];
        return variant && variant->szInternalName ? variant->szInternalName : std::string_view{};
    }

    std::string GetDisplayName(const WeaponVariantDef& variant)
    {
        if (variant.szDisplayName && *variant.szDisplayName)
        {
            typedef const char*(__cdecl * SEH_StringEd_GetString_t)(const char* reference);
            const auto translate =
                reinterpret_cast<SEH_StringEd_GetString_t>(GetGameAddresses().SEH_StringEd_GetString());
            if (const auto translated = translate(variant.szDisplayName); translated && *translated)
                return translated;
        }
        return variant.szInternalName;
    }

    std::vector<std::string> GetLoadedModels(std::string_view prefix, std::string_view part)
    {
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());

        std::vector<std::string> models;
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
        {
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
            {
                const auto& entry = entries[index];
                if (entry.type != ASSET_TYPE_XMODEL || !entry.header)
                    continue;
                const std::string_view name = *static_cast<const char* const*>(entry.header);
                if (name.starts_with(prefix) && name.find(part) != std::string_view::npos)
                    models.emplace_back(name);
            }
        }
        std::ranges::sort(models);
        return models;
    }

    // what the map's assets offer; called on the main thread once the demo has loaded
    void BuildCatalog()
    {
        // the game takes face paint off beyond 1250 units; a long lens sees it from further (this dvar isn't saved)
        if (const auto facepaintLodDist = Functions::FindDvar("facepaintLodDist"))
            facepaintLodDist->current.value = 100000.0f;

        auto catalog = std::make_shared<Types::LoadoutCatalog>();
        listedVariants = {};
        variantIndices.clear();

        std::unordered_map<std::string, std::string> categories;
        for (const auto& [category, weapons] : CATEGORIES)
            for (const auto weapon : weapons)
                categories.emplace(weapon, category);

        // variants come right after their base weapon
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        std::set<std::string> attachmentsSeen;
        std::map<std::string, Types::LoadoutCatalog::Weapon> weapons;
        for (std::uint16_t index = 1; index < Functions::GetWeaponVariantCount(); ++index)
        {
            const auto variant = variants[index];
            if (!variant)
                break;
            const std::string_view name = variant->szInternalName ? variant->szInternalName : "";

            const auto baseIndex = variant->baseOrVariantCount < 0 ? -variant->baseOrVariantCount : index;
            const auto base = baseIndex < MAX_WEAPON_VARIANTS ? variants[baseIndex] : nullptr;
            if (!base || !base->szInternalName || !name.ends_with("_mp"))
                continue;

            std::string stem(base->szInternalName);
            if (!stem.ends_with("_mp"))
                continue;
            stem.resize(stem.size() - 3);

            std::vector<std::string> attachments;
            if (stem.ends_with("dw"))
            {
                stem.resize(stem.size() - 2);
                attachments.emplace_back("dw");
            }
            const auto category = categories.find(stem);
            if (category == categories.end())
                continue;

            // <stem>[dw]_<attachment>_..._mp; alternate modes like gl_<stem>_mp are left alone
            const std::string_view baseName = base->szInternalName;
            if (name != baseName)
            {
                const auto prefix = baseName.substr(0, baseName.size() - 3);
                if (!name.starts_with(prefix) || name[prefix.size()] != '_')
                    continue;
                auto parts = name.substr(prefix.size() + 1, name.size() - prefix.size() - 4);
                while (!parts.empty())
                {
                    const auto end = std::min(parts.find('_'), parts.size());
                    attachments.emplace_back(parts.substr(0, end));
                    parts.remove_prefix(std::min(end + 1, parts.size()));
                }
            }
            std::ranges::sort(attachments);
            attachmentsSeen.insert(attachments.begin(), attachments.end());

            const auto weaponName = stem + "_mp";
            const bool actorsOnly = ACTORS_ONLY_CATEGORIES.contains(category->second);
            if (attachments.empty())
                weapons[weaponName] = {.name = weaponName,
                                       .displayName = GetDisplayName(*base),
                                       .category = category->second,
                                       .primary = !SECONDARY_CATEGORIES.contains(category->second),
                                       .actorsOnly = actorsOnly,
                                       .fireTime = base->weapDef ? static_cast<std::uint32_t>(base->weapDef->iFireTime) : 0,
                                       .aimedSpread = base->weapDef ? base->weapDef->adsSpread : 0.0f};

            catalog->variants.push_back({std::string(name), weaponName, std::move(attachments), actorsOnly});
            // (what isn't listed keeps its look on a player)
            if (!actorsOnly)
                listedVariants[index] = name;
            variantIndices.emplace(name, index);
        }

        // weapons in the order of their categories
        for (const auto& [category, names] : CATEGORIES)
            for (const auto stem : names)
                if (const auto weapon = weapons.find(std::string(stem) + "_mp"); weapon != weapons.end())
                    catalog->weapons.push_back(weapon->second);

        for (const auto& [name, displayName] : ATTACHMENTS)
            if (attachmentsSeen.contains(std::string(name)))
                catalog->attachments.push_back({std::string(name), std::string(displayName)});
        for (const auto& name : attachmentsSeen)
            if (std::ranges::find(ATTACHMENTS, name, &std::pair<std::string_view, std::string_view>::first) ==
                ATTACHMENTS.end())
                catalog->attachments.push_back({name, name});

        // the campaign's and Zombies' weapons and the Pack-a-Punch ones, which actors hold from zones of their own:
        // those not built greyed, with why
        const auto& campaignWeapons = Components::WeaponCatalog::Get();
        Components::WeaponCatalog::AddTo(*catalog, campaignWeapons, [&](const std::string& zone) -> std::string {
            if (Components::Actors::IsCharacterZoneAvailable(zone))
                return {};
            const auto entry = campaignWeapons.FindZone(zone);
            const bool levelThere = entry && std::ranges::any_of(entry->sources, [](const std::string& level) {
                                        return ContentPacks::HasGameZone(level);
                                    });
            return levelThere ? "Run Iris Studio to add it" : "Not in this game folder";
        });

        catalog->camos = CAMOS;
        catalog->bodies = GetLoadedModels("c_", "_body");
        catalog->heads = GetLoadedModels("c_", "_head");

        builtCatalog = catalog;
        Components::Loadouts::SetCatalog(std::move(catalog));
    }

    // What a weapon and camo turn into, kept until the choices change
    struct WeaponKey
    {
        std::int32_t client;
        std::uint16_t weapon;
        std::uint8_t camo;
        auto operator<=>(const WeaponKey&) const = default;
    };
    std::map<WeaponKey, std::pair<std::uint16_t, std::uint8_t>> resolvedWeapons;
    std::array<Components::PlayerLookLogic::ResolvedCharacter, MAX_CLIENTS> resolvedCharacters;
    std::optional<std::uint32_t> resolvedGeneration;
    const void* resolvedEvents = nullptr;

    void UpdateResolved()
    {
        const auto generation = Components::Loadouts::GetGeneration();
        if (resolvedGeneration == generation)
            return;
        resolvedGeneration = generation;
        resolvedWeapons.clear();
        for (std::size_t client = 0; client < MAX_CLIENTS; ++client)
            resolvedCharacters[client] = Components::Loadouts::ResolveCharacter(static_cast<std::int32_t>(client));
    }

    std::pair<std::uint16_t, std::uint8_t> ResolveWeapon(std::int32_t client, std::uint16_t weapon, std::uint8_t camo)
    {
        const WeaponKey key{client, weapon, camo};
        if (const auto it = resolvedWeapons.find(key); it != resolvedWeapons.end())
            return it->second;

        std::pair result(weapon, camo);
        if (weapon < MAX_WEAPON_VARIANTS && !listedVariants[weapon].empty())
        {
            if (const auto resolved = Components::Loadouts::ResolveWeapon(client, listedVariants[weapon], camo))
            {
                if (const auto index = variantIndices.find(resolved->variant); index != variantIndices.end())
                    result.first = index->second;
                result.second = static_cast<std::uint8_t>(resolved->camo & RENDER_OPTIONS_CAMO_BITS);
            }
        }
        resolvedWeapons.emplace(key, result);
        return result;
    }

    void ApplyWeapon(std::int32_t client, std::uint16_t& weapon, std::uint16_t* secondWeapon,
                     std::uint32_t& renderOptions)
    {
        if (weapon == 0)
            return;
        const auto camo = static_cast<std::uint8_t>(renderOptions & RENDER_OPTIONS_CAMO_BITS);
        const auto [newWeapon, newCamo] = ResolveWeapon(client, weapon, camo);
        if (secondWeapon && *secondWeapon == weapon)
            *secondWeapon = newWeapon;
        weapon = newWeapon;
        renderOptions = (renderOptions & ~RENDER_OPTIONS_CAMO_BITS) | newCamo;
    }

    namespace Look = Components::PlayerLookLogic;

    // The demo's models of each player (the body, then what is attached to it), the ones written over them, and how
    // often they were written, which tells the players' models when to be built again
    struct CharacterModels
    {
        Look::Models original;
        std::optional<Look::Models> written;
        Components::Loadouts::LeftOut leftOut;
    };
    std::array<CharacterModels, MAX_CLIENTS> characterModels;
    std::array<std::uint32_t, MAX_CLIENTS> characterVersions{};
    std::array<CharacterModels, MAX_CORPSES> corpseModels;
    std::array<std::uint32_t, MAX_CORPSES> corpseVersions{};

    Look::Models ReadModels(const clientInfo_t& info)
    {
        Look::Models models{{std::string(info.model, strnlen(info.model, sizeof(info.model))), {}}};
        for (std::size_t i = 0; i < std::size(info.attachModelNames) && info.attachModelNames[i][0]; ++i)
            models.push_back({info.attachModelNames[i], info.attachTagNames[i]});
        return models;
    }

    void WriteModels(clientInfo_t& info, const Look::Models& models)
    {
        strcpy_s(info.model, models.front().name.c_str());
        for (std::size_t i = 0; i < std::size(info.attachModelNames); ++i)
        {
            const bool used = i + 1 < models.size();
            strcpy_s(info.attachModelNames[i], used ? models[i + 1].name.c_str() : "");
            strcpy_s(info.attachTagNames[i], used ? models[i + 1].tag.c_str() : "");
        }
    }

    // the bones of a weapon in a player's hands, and of a dual wield gun's left one
    std::uint32_t CountGunBones(std::uint16_t weapon)
    {
        const auto variants = reinterpret_cast<WeaponVariantDef**>(GetGameAddresses().bg_weaponVariantDefs());
        const auto variant = weapon < MAX_WEAPON_VARIANTS ? variants[weapon] : nullptr;
        const auto count = [](const WeaponVariantDef* gun) -> std::uint32_t {
            const auto model = gun && gun->weapDef && gun->weapDef->worldModel ? gun->weapDef->worldModel[0] : nullptr;
            return model ? model->numBones : 0;
        };
        return count(variant) + (variant ? count(WeaponRig::FindLeftHandVariant(*variant)) : 0);
    }

    // The game drops out of the demo when a player's models have more bones together than a DObj can (see
    // MAX_DOBJ_BONES), and a campaign character has many more than a multiplayer one. Null for a model that isn't
    // loaded.
    std::optional<Look::Models> FitModels(const Look::Models& models, std::uint16_t weapon)
    {
        std::vector<std::uint32_t> bones;
        for (const auto& model : models)
        {
            const auto xmodel =
                static_cast<const XModel*>(ContentPacks::FindAsset(ASSET_TYPE_XMODEL, model.name.c_str()));
            if (!xmodel)
                return std::nullopt;
            bones.push_back(xmodel->numBones);
        }
        auto added = CountGunBones(weapon);
        const auto body = static_cast<XModel*>(ContentPacks::FindAsset(ASSET_TYPE_XMODEL, models[0].name.c_str()));
        if (const auto rig = Actors::GetSpineRigOf(*Actors::GetPlayerBody(Actors::GetHoldingModel(body))))
            added += rig->numBones;
        return Look::Fit(models, bones, added, MAX_DOBJ_BONES);
    }

    // the model of the stowed gun chosen, empty for none; nullopt to keep the demo's
    std::optional<std::string> GetStowedModelName(const Look::ResolvedCharacter& chosen)
    {
        if (!chosen.stowed)
            return std::nullopt;
        const auto variant = chosen.stowed->empty() ? nullptr : WeaponRig::FindVariant(*chosen.stowed);
        const auto model = variant ? WeaponRig::GetStowedModel(*variant) : nullptr;
        return model ? std::string(model->name) : std::string();
    }

    // Writes the chosen models into a clientInfo_t; returns whether it has models of the choices (rather than the
    // demo's). Those of a character zone come once it has loaded, all of them at once.
    bool ApplyModels(clientInfo_t& info, const Look::ResolvedCharacter& chosen, CharacterModels& models,
                     std::uint16_t weapon, std::uint32_t& version)
    {
        if (!chosen.HasModels() && !models.written)
            return false;

        // the game wrote the demo's models again, e.g. after a rewind
        const auto current = ReadModels(info);
        if (!models.written || current != *models.written)
        {
            models.original = current;
            models.written.reset();
        }

        auto wanted = Look::GetWanted(models.original, chosen, std::size(info.attachModelNames) + 1,
                                      GetStowedModelName(chosen));
        models.leftOut = {};
        if (wanted != models.original)
        {
            const auto fitted = FitModels(wanted, weapon);
            for (const auto& model : wanted)
                if (fitted && std::ranges::find(*fitted, model) == fitted->end())
                    (model.tag.empty() ? models.leftOut.worn : models.leftOut.stowed) = true;
            wanted = fitted.value_or(models.written.value_or(models.original));
        }

        if (current != wanted)
        {
            WriteModels(info, wanted);
            info.dobjDirty = 1;
            ++version;
        }
        if (wanted != models.original)
            models.written = wanted;
        else
            models.written.reset();
        return models.written.has_value();
    }

    bool ApplyCharacter(const entityState_s& es)
    {
        const auto client = es.clientNum;
        if (!builtCatalog || static_cast<std::uint32_t>(client) >= MAX_CLIENTS)
            return false;

        if (es.eType == ET_PLAYER)
        {
            const auto cg = GetClientGlobals();
            if (!cg || !cg->clientInfo[client].infoValid)
                return false;
            auto& models = characterModels[client];
            const auto before = models.leftOut;
            const bool applied = ApplyModels(cg->clientInfo[client], resolvedCharacters[client], models,
                                             es.primaryWeapon, characterVersions[client]);
            if (models.leftOut.stowed != before.stowed || models.leftOut.worn != before.worn)
                Components::Loadouts::SetLeftOut(client, models.leftOut);
            return applied;
        }

        // The game copies a player's clientInfo_t into its corpse's as the corpse shows up, but only once for a corpse
        // of the same player, so one seen before keeps what the player had then (a rewind brings it back)
        const auto corpse = static_cast<std::uint32_t>(es.number) - FIRST_CORPSE_ENTITY;
        const auto info = corpse < MAX_CORPSES ? GetCorpseInfo(corpse) : nullptr;
        if (!info)
            return false;
        return ApplyModels(*info, resolvedCharacters[client], corpseModels[corpse], es.primaryWeapon,
                           corpseVersions[corpse]);
    }

    // The face paint a player or corpse is drawn with is the top of its render options (see Types::Loadout::facePaint)
    void ApplyFacePaint(std::uint32_t& renderOptions, std::int32_t client)
    {
        if (static_cast<std::uint32_t>(client) >= MAX_CLIENTS)
            return;
        if (const auto paint = resolvedCharacters[client].facePaint)
            renderOptions = (renderOptions & ~RENDER_OPTIONS_FACE_PAINT_BITS) |
                            (static_cast<std::uint32_t>(*paint) << RENDER_OPTIONS_FACE_PAINT_SHIFT);
    }

    // The game builds a player's or corpse's model again only when the parts the entity state hides differ from the
    // model's, and then works them out into the state. So a changed look starts with parts that can't match, and the
    // parts worked out for it are put back into every new snapshot's state, which has the demo's.
    struct BuiltModel
    {
        std::uint16_t weapon;
        std::uint8_t camo;
        std::uint32_t characterVersion;
        std::array<std::uint32_t, 5> partBits;

        bool IsFor(const BuiltModel& other) const
        {
            return weapon == other.weapon && camo == other.camo && characterVersion == other.characterVersion;
        }
    };
    std::array<std::optional<BuiltModel>, MAX_GENTITIES> builtModels;

    // the demo's values of the state that is being drawn, put back once its model is updated
    struct DemoState
    {
        std::uint16_t primaryWeapon, weapon, stowedWeapon;
        std::uint32_t renderOptions;
        bool changed;
        BuiltModel model;
    };
    std::array<std::optional<DemoState>, MAX_GENTITIES> demoStates;

    void ProcessEntity(centity_t& cent)
    {
        // (a live game's bots have the game's look)
        if (!builtCatalog || Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            return;

        auto& es = cent.nextState;
        const auto number = static_cast<std::uint32_t>(es.number);
        if ((es.eType != ET_PLAYER && es.eType != ET_PLAYER_CORPSE) || number >= MAX_GENTITIES)
            return;

        UpdateResolved();
        DemoState demo{es.primaryWeapon, es.weapon, es.stowedWeapon, es.renderOptions};

        ApplyWeapon(es.clientNum, es.primaryWeapon, &es.weapon, es.renderOptions);
        const bool character = ApplyCharacter(es);
        // (the stowed gun's attachments are hidden by what the entity says it is)
        if (static_cast<std::uint32_t>(es.clientNum) < MAX_CLIENTS)
            if (const auto& stowed = resolvedCharacters[es.clientNum].stowed)
            {
                const auto index = variantIndices.find(*stowed);
                es.stowedWeapon = index != variantIndices.end() ? index->second : std::uint16_t{0};
            }
        ApplyFacePaint(es.renderOptions, es.clientNum);

        demo.changed = character || es.primaryWeapon != demo.primaryWeapon || es.renderOptions != demo.renderOptions;
        auto& built = builtModels[number];
        if (!demo.changed && !built)
            return;

        const auto corpse = number - FIRST_CORPSE_ENTITY;
        demo.model = {es.primaryWeapon, static_cast<std::uint8_t>(es.renderOptions & RENDER_OPTIONS_CAMO_BITS),
                      es.eType == ET_PLAYER && es.clientNum < MAX_CLIENTS ? characterVersions[es.clientNum]
                      : es.eType == ET_PLAYER_CORPSE && corpse < MAX_CORPSES ? corpseVersions[corpse]
                                                                             : 0};
        if (built && built->IsFor(demo.model))
            std::ranges::copy(built->partBits, es.partBits);
        else
            std::ranges::fill(es.partBits, 0xFFFFFFFF);
        demoStates[number] = demo;
    }

    void ProcessEntityModel(centity_t& cent)
    {
        auto& es = cent.nextState;
        const auto number = static_cast<std::uint32_t>(es.number);
        if (number >= MAX_GENTITIES || !demoStates[number])
            return;

        auto& demo = *demoStates[number];
        auto& built = builtModels[number];
        if (demo.changed)
        {
            built = demo.model;
            std::ranges::copy(es.partBits, built->partBits.begin());
        }
        else
        {
            // built from the demo's state again, which needs nothing more
            built.reset();
        }

        es.primaryWeapon = demo.primaryWeapon;
        es.weapon = demo.weapon;
        es.stowedWeapon = demo.stowedWeapon;
        es.renderOptions = demo.renderOptions;
        demoStates[number].reset();
    }

    // The demo's own player in first person: the view model shows the player state's weapon, with the camo of its
    // entry in the held weapons, from which the game also builds the view models when a snapshot comes in. These are
    // replaced in the snapshots the player state is worked out from, which keep what was written until they are
    // filled with a newer snapshot.
    struct PlayerStateWeapons
    {
        std::uint16_t weapon;
        std::uint32_t renderOptions;
        std::array<std::pair<std::uint32_t, std::uint32_t>, MAX_HELD_WEAPONS> held;  // weapon, render options

        bool operator==(const PlayerStateWeapons&) const = default;
    };
    struct SwappedPlayerState
    {
        PlayerStateWeapons demo, applied;
    };
    std::map<const playerState_s*, SwappedPlayerState> swappedPlayerStates;

    PlayerStateWeapons ReadWeapons(const playerState_s& ps)
    {
        PlayerStateWeapons weapons{ps.weapon, ps.renderOptions};
        for (std::size_t i = 0; i < MAX_HELD_WEAPONS; ++i)
            weapons.held[i] = {ps.heldWeapons[i].weapon, ps.heldWeapons[i].renderOptions};
        return weapons;
    }

    void ApplyToPlayerState(playerState_s& ps)
    {
        const auto current = ReadWeapons(ps);
        const auto swapped = swappedPlayerStates.find(&ps);
        const auto demo = swapped != swappedPlayerStates.end() && swapped->second.applied == current
                              ? swapped->second.demo
                              : current;

        auto applied = demo;
        std::optional<std::size_t> heldWeapon;
        for (std::size_t i = 0; i < MAX_HELD_WEAPONS; ++i)
        {
            auto& [weapon, renderOptions] = applied.held[i];
            if (weapon == 0 || weapon >= MAX_WEAPON_VARIANTS)
                continue;
            if (weapon == demo.weapon)
                heldWeapon = i;
            auto newWeapon = static_cast<std::uint16_t>(weapon);
            ApplyWeapon(ps.clientNum, newWeapon, nullptr, renderOptions);
            weapon = newWeapon;
            ApplyFacePaint(renderOptions, ps.clientNum);
        }
        // the entity the game draws this player from takes its options from these, not the weapon's, which held the
        // paint only for the view model's sake: without this, the player showed it only as a corpse
        ApplyFacePaint(applied.renderOptions, ps.clientNum);
        if (heldWeapon)
        {
            applied.weapon = static_cast<std::uint16_t>(applied.held[*heldWeapon].first);
        }
        else
        {
            std::uint32_t renderOptions = 0;
            ApplyWeapon(ps.clientNum, applied.weapon, nullptr, renderOptions);
        }

        if (applied != current)
        {
            ps.weapon = applied.weapon;
            ps.renderOptions = applied.renderOptions;
            for (std::size_t i = 0; i < MAX_HELD_WEAPONS; ++i)
                std::tie(ps.heldWeapons[i].weapon, ps.heldWeapons[i].renderOptions) = applied.held[i];
        }
        swappedPlayerStates[&ps] = {demo, applied};
    }

    std::optional<std::uint32_t> viewModelGeneration;

    void ProcessPredictedPlayer()
    {
        const auto cg = GetClientGlobals();
        const bool inDemo = Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo;
        if (!cg || (!inDemo && !LiveActors::IsLive()))
            return;

        if (!builtCatalog)
            BuildCatalog();
        if (!inDemo)
            return;

        // (the scan of the demo tells who is on which team)
        if (const auto events = Mod::GetGameInterface()->GetDemoEvents(); events.get() != resolvedEvents)
        {
            resolvedEvents = events.get();
            resolvedGeneration.reset();
        }

        // new snapshots are changed as they come in (see CL_GetSnapshot_Hook), the ones the game has when the choices
        // change here
        UpdateResolved();
        for (const auto snapshot : {cg->snap, cg->nextSnap})
            if (snapshot)
                ApplyToPlayerState(snapshot->ps);

        // the view model is built with the camo of its gun, and built again only for another gun
        if (viewModelGeneration != resolvedGeneration)
        {
            viewModelGeneration = resolvedGeneration;
            cg->viewModelWeapon = 0;
        }
    }

    // A snapshot that comes in is worked into the player state right away, before the next frame is drawn, so it is
    // changed as soon as the game has copied it
    typedef bool(__cdecl* CL_GetSnapshot_t)(int localClientNum, int snapshotNumber, snapshot_t* snapshot);
    CL_GetSnapshot_t CL_GetSnapshot_Trampoline;
    bool __cdecl CL_GetSnapshot_Hook(int localClientNum, int snapshotNumber, snapshot_t* snapshot)
    {
        const bool result = CL_GetSnapshot_Trampoline(localClientNum, snapshotNumber, snapshot);
        if (result && snapshot)
            CampaignMaps::FixSnapshot(*snapshot);
        if (result && snapshot && builtCatalog &&
            Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            UpdateResolved();
            ApplyToPlayerState(snapshot->ps);
        }
        return result;
    }

    // Builds the model of each player of a new snapshot, the demo's own player included, which CG_AddPacketEntity
    // doesn't build
    typedef void(__cdecl* CG_UpdatePlayerModel_t)(int localClientNum, centity_t* cent);
    CG_UpdatePlayerModel_t CG_UpdatePlayerModel_Trampoline;
    void __cdecl CG_UpdatePlayerModel_Hook(int localClientNum, centity_t* cent)
    {
        const auto entities = GetEntities();
        const bool entity = entities && cent >= entities && cent < entities + MAX_GENTITIES;
        if (entity)
            ProcessEntity(*cent);

        CG_UpdatePlayerModel_Trampoline(localClientNum, cent);

        if (entity)
            ProcessEntityModel(*cent);
    }

    // Animations made for MP bodies (all of the players') need torso_stabilizer on a campaign body, or its torso turns by
    // about 90 degrees: the body's copy with it (Actors::GetPlayerBody) goes in as the game makes a player's or corpse's
    // model (the game's other entities' numbers are below the actors' handles), or for a body that can't have one, the
    // spine rig in front of it (see Actors.cpp), without the game's IK. With the rig the game goes by the models' indices
    // only for the parts of the stowed weapon, which then shows its attachments.
    typedef DObj*(__cdecl* Com_ClientDObjCreate_t)(DObjModel_s* models, std::uint32_t numModels, XAnimTree_s* tree,
                                                   std::int32_t handle, std::int32_t localClientNum);
    Com_ClientDObjCreate_t Com_ClientDObjCreate_Trampoline;
    DObj* __cdecl Com_ClientDObjCreate_Hook(DObjModel_s* models, std::uint32_t numModels, XAnimTree_s* tree,
                                            std::int32_t handle, std::int32_t localClientNum)
    {
        constexpr std::uint32_t MOST_MODELS = 16;
        // (by number: a player's first model can come before its entity state, as a live game restarts)
        const bool player = handle >= 0 && static_cast<std::uint32_t>(handle) < FIRST_CORPSE_ENTITY + MAX_CORPSES;
        if (player && tree && numModels > 0 && numModels < MOST_MODELS && models[0].model)
        {
            std::array<DObjModel_s, MOST_MODELS> fitted;
            std::copy_n(models, numModels, fitted.begin());
            // as an actor's, a body that rests the gun tag elsewhere has the gun where the animations have it
            for (std::uint32_t i = 0; i < numModels; ++i)
                if (fitted[i].model)
                    fitted[i].model = Actors::GetHoldingModel(fitted[i].model);
            fitted[0].model = Actors::GetPlayerBody(fitted[0].model);
            if (const auto rig = Actors::GetSpineRigOf(*fitted[0].model))
            {
                std::array<DObjModel_s, MOST_MODELS> withRig;
                withRig[0] = {const_cast<XModel*>(rig), 0, false};
                std::copy_n(fitted.begin(), numModels, withRig.begin() + 1);
                return Com_ClientDObjCreate_Trampoline(withRig.data(), numModels + 1, tree, handle, localClientNum);
            }
            return Com_ClientDObjCreate_Trampoline(fitted.data(), numModels, tree, handle, localClientNum);
        }
        return Com_ClientDObjCreate_Trampoline(models, numModels, tree, handle, localClientNum);
    }

    void ResetLive()
    {
        builtCatalog.reset();
        characterModels = {};
        characterVersions = {};
        corpseModels = {};
        corpseVersions = {};
        builtModels = {};
        demoStates = {};
        swappedPlayerStates.clear();
        viewModelGeneration.reset();
        resolvedGeneration.reset();
    }

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().CG_UpdatePlayerModel(), (std::uintptr_t)CG_UpdatePlayerModel_Hook,
                                (std::uintptr_t*)&CG_UpdatePlayerModel_Trampoline);
        HookManager::CreateHook(GetGameAddresses().Com_ClientDObjCreate(), (std::uintptr_t)Com_ClientDObjCreate_Hook,
                                (std::uintptr_t*)&Com_ClientDObjCreate_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CL_GetSnapshot(), (std::uintptr_t)CL_GetSnapshot_Hook,
                                (std::uintptr_t*)&CL_GetSnapshot_Trampoline);

        // the weapons and models are assets of the map
        Events::RegisterListener(EventType::PreDemoLoad, [] {
            builtCatalog.reset();
            Components::Loadouts::SetCatalog(nullptr);
        });
        Events::RegisterListener(EventType::PostDemoLoad, ResetLive);
    }
}  // namespace IWXMVM::T5::Hooks::Loadouts
