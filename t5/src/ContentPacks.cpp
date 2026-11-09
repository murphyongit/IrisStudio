#include "StdInclude.hpp"
#include "ContentPacks.hpp"
#include "ContentPacksInternal.hpp"

#include <Psapi.h>
#include <bitset>
#include <unordered_map>
#include <unordered_set>

#include "Events.hpp"
#include "Mod.hpp"
#include "Actors.hpp"
#include "AnimationStore.hpp"
#include "CampaignMaps.hpp"
#include "PoolGrowth.hpp"
#include "LiveActors.hpp"
#include "LiveGame.hpp"
#include "MapMenu.hpp"
#include "WeaponZones.hpp"
#include "Setup/SetupLogic.hpp"
#include "Components/Actors.hpp"
#include "Components/AddressSpaceLogic.hpp"
#include "Components/WeaponCatalog.hpp"
#include "Components/CaptureManager.hpp"
#include "Functions.hpp"
#include "Utilities/HookManager.hpp"
#include "Utilities/Patches.hpp"
#include "Addresses.hpp"
#include "Structures.hpp"

namespace IWXMVM::T5::ContentPacks
{
    using namespace Structures;

    // Multiplayer can load any zone of the game (they share the format), but a single player level brings its whole
    // world along, and the pools for images, materials and models are almost full with the map loaded, so loading
    // one fails with "Exceeded limit of 4608 'image' assets". Images and materials can't get room outside their
    // pools either: the renderer finds an image by its position in the pool, and sorts the materials of the pool.
    //
    // So a pack is loaded twice. While it loads, DB_AddXAsset, which every asset goes through to be registered, is
    // hooked. The first time nothing is registered, and the character models (named c_*) are walked for the
    // materials, techniques and images they draw with. The second time only those are registered, by name, wherever
    // in the zone they come from (the world often shares them). Everything else still loads, since the zone's data
    // has to be read in order, but nothing can find it. The single player common zone is loaded (once, in a slot of
    // its own) for its animations (those of the AI), and only they are registered. (Loading both zones in one go
    // left the weapon table broken.)
    //
    // The loader reads each asset's header into a block it reuses for the next one and registering copies it into
    // the pool, so an asset left out gets a copy of its header here instead, which what refers to it keeps pointing
    // to (the rest of it is in the zone's memory). The copies live as long as the pack.
    //
    // Sound banks and patches are also handed to the sound system once added, which would keep the copies after the
    // zone is gone (and play the single player sounds), so they are taken out of its lists right after loading.
    //
    // An asset needed with the name of one already registered (most often a technique or an image the multiplayer
    // characters use too) isn't registered again: that would replace the registered one in place, for everything
    // using it (a single player technique drawing a multiplayer model failed on its vertex format). The pack uses
    // the registered one instead.
    //
    // Models, techniques and images also get their vertex buffers, shaders and textures as they load, which unloading
    // the zone only releases for the assets it registered, so they are released for those left out (a level's worth)
    // right after loading, with the game's own unload function for the type. (The world and collision ones release
    // nothing; they end the game if their map is in use.) A world's vertex and index buffers are created as it
    // loads and only released when its map is unloaded, so they are found in its header and released here.

    enum class Pass
    {
        None,
        Survey,
        Load,
    };
    Pass pass = Pass::None;
    bool loading = false;  // while a pack or character zone loads
    LoadResult* result = nullptr;
    std::mutex packMutex;
    PackState packState;
    std::shared_mutex assetMutex;  // held to load zones, and by other threads reading assets meanwhile
    bool unloaded = false;         // the mod unloads (see Unload)

    enum class Keep
    {
        Characters,
        Animations,
        All,  // a character zone
    };
    struct PackZone
    {
        std::string name;
        Keep keep;
    };
    std::vector<PackZone> packZones;

    // the zone single player animations come from
    inline constexpr std::string_view ANIMATIONS_ZONE = "common";

    struct XZoneInfo
    {
        const char* name;
        std::int32_t allocFlags;
        std::int32_t freeFlags;
    };
    // what the loadzone command loads with, so it replaces a pack (and a pack replaces what it loaded)
    inline constexpr std::int32_t PACK_ZONE_FLAGS = 0x200000;
    // a flag no zone of the game loads with
    inline constexpr std::int32_t ANIMATIONS_ZONE_FLAGS = 0x10000000;
    bool animationsLoaded = false;

    // Character zones are loaded into memory of their own (allocation flag 0x40000000, freed zone by zone) rather than
    // the game's physical memory, which only frees what came last (see UnloadZonesWithClient), so any of them can be
    // unloaded while others stay. DB_LoadXAssets unloads zones by flag bits, and with the top bit of the free flags
    // set it unloads the zones of those bits alone (else also those of the bits it counts as loaded after them). Of
    // the bits it unloads by, these are the ones no zone of multiplayer loads with (nor its language twins, which
    // the loader looks for in the language folder): each character zone gets one, shared when there are more zones.
    // Not 0x20: a game mod's mod.ff (and Plutonium's mod1-mod5) load with it, and unloading it with the client's
    // teardown ended the game at STOP with "Disconnected from server".
    inline constexpr std::int32_t CHARACTER_ZONE_MEMORY = 0x40000000;
    inline constexpr std::int32_t FREE_EXACTLY = static_cast<std::int32_t>(0x80000000);
    inline constexpr std::array<std::int32_t, 5> CHARACTER_ZONE_BITS{0x4, 0x100, 0x200, 0x4000, 0x10000};
    // The zones of the weapons the game registers keep a bit of their own: they stay until the map goes (see
    // HasRegisteredWeapon), and on a shared bit they would keep every character zone of it loaded too. A film of 15 guns
    // left no bit free, so no actor's character could go.
    inline constexpr std::int32_t REGISTERED_WEAPON_ZONE_BIT = 0x20000;
    inline constexpr std::int32_t ALL_CHARACTER_ZONE_BITS =
        0x4 | 0x100 | 0x200 | 0x4000 | 0x10000 | REGISTERED_WEAPON_ZONE_BIT;
    // the bit 0x100 replaced, which an older T5iris injected earlier into this game put in the teardown's free flags
    inline constexpr std::int32_t MOD_ZONE_FLAGS = 0x20;

    std::vector<XModel*> surveyedModels;
    std::vector<std::unique_ptr<std::uint8_t[]>> headerCopies;
    std::unordered_set<const void*> copies;
    std::vector<void*> soundCopies;
    std::vector<std::pair<std::int32_t, void*>> unloadableCopies;
    // the models and technique sets the pack registered
    std::vector<XModel*> packModels;
    std::unordered_set<const MaterialTechniqueSet*> packTechniqueSets;

    // A character zone loaded (see LoadCharacterZone)
    struct CharacterZone
    {
        std::string name;
        std::int32_t flags = 0;
        std::vector<std::string> models;
        std::vector<const GfxImage*> images;                            // it registered
        std::unordered_set<const MaterialTechniqueSet*> techniqueSets;  // it registered
        std::set<std::string> dependsOn;                                // the character zones whose assets it uses
        bool usesPack = false;
        std::vector<std::unique_ptr<std::uint8_t[]>> headerCopies;  // of what had no room in its pool (see KeepHeader)
        // the tables made for its materials (see MakeDrawable)
        std::vector<std::unique_ptr<MaterialConstantDef[]>> constantTables;
        std::vector<std::unique_ptr<MaterialTextureDef[]>> textureTables;
        // a weapon zone's weapons (WeaponVariantDef) and a sound zone's bank it registered
        std::vector<const void*> weapons;
        mutable bool weaponRegistered = false;  // seen so (see HasRegisteredWeapon), which holds until the zone goes
        void* soundBank = nullptr;

        CharacterZone() = default;
        CharacterZone(CharacterZone&&) = default;
        CharacterZone& operator=(CharacterZone&&) = default;
        CharacterZone(const CharacterZone&) = delete;
    };
    std::vector<CharacterZone> characterZones;  // in the order they were loaded
    std::unique_ptr<CharacterZone> loadingCharacterZone;
    // The sound zones loaded (a level's sound bank, which has the sounds of its weapons' shots: t5s_*). A sound bank
    // unloaded from the character zones' own memory corrupted the heap (the game ended with 0xC0000374, at STOP or
    // any unload of one), while one in the game's zone memory, where packs and the loadzone command load, unloads as
    // the game's own banks do. So they load there (PACK_ZONE_FLAGS, freeing nothing as they load) and stay until the
    // game frees them with the client at STOP or with the map, the last loaded first, as it frees packs; the mod never
    // unloads one itself.
    std::vector<CharacterZone> soundZones;

    // a technique set the pack or a character zone registered (single player's, where the others are multiplayer's)
    bool IsOwnTechniqueSet(const MaterialTechniqueSet* techniqueSet)
    {
        return packTechniqueSets.contains(techniqueSet) ||
               (loadingCharacterZone && loadingCharacterZone->techniqueSets.contains(techniqueSet)) ||
               std::ranges::any_of(characterZones,
                                   [&](const auto& zone) { return zone.techniqueSets.contains(techniqueSet); });
    }

    // the name of a loaded zone (an asset entry's zoneIndex)
    std::string_view GetZoneName(std::uint32_t zoneIndex)
    {
        return reinterpret_cast<const char*>(PoolGrowth::GetZoneInfo() + zoneIndex * ZONE_INFO_SIZE);
    }

    CharacterZone* FindSoundZone(std::string_view name)
    {
        const auto it = std::ranges::find(soundZones, name, &CharacterZone::name);
        return it != soundZones.end() ? &*it : nullptr;
    }

    CharacterZone* FindCharacterZone(std::string_view name)
    {
        const auto zone = std::ranges::find_if(characterZones, [&](const auto& z) { return z.name == name; });
        return zone != characterZones.end() ? &*zone : nullptr;
    }

    bool HasResources(std::int32_t type)
    {
        return type == ASSET_TYPE_XMODEL || type == ASSET_TYPE_TECHNIQUE_SET || type == ASSET_TYPE_IMAGE ||
               type == ASSET_TYPE_GFXWORLD;
    }

    // the code of the Direct3D 9 runtime and of what stands in for it (ReShade's d3d9.dll)
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> GetDirect3DModules()
    {
        std::vector<std::pair<std::uintptr_t, std::uintptr_t>> modules;
        HMODULE handles[1024];
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed))
            return modules;
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < std::size(handles); ++i)
        {
            char path[MAX_PATH]{};
            MODULEINFO info{};
            if (!GetModuleFileNameA(handles[i], path, MAX_PATH) ||
                !GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)))
                continue;
            if (_stricmp(std::filesystem::path(path).filename().string().c_str(), "d3d9.dll") == 0)
                modules.emplace_back(reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll),
                                     reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll) + info.SizeOfImage);
        }
        return modules;
    }

    bool IsReadable(const void* address, std::size_t size);

    // The device isn't made for use from several threads (behaviour flags 0x40), and the render thread uses it between
    // frames too (it streams images in, and runs what other threads queue for it). Releasing Direct3D objects on
    // another thread meanwhile broke the driver now and then (it hung or crashed later), so the mod's releases are
    // queued for the render thread, as the game queues a technique set's shaders unloaded off it.
    // Every so many the main thread waits until the render thread has run what was queued, which keeps the queue
    // short (a wait takes a turn of the render thread's loop, about 5 ms, so a pack's hundreds of models made apiece
    // took seconds).
    inline constexpr std::uint32_t RELEASES_QUEUED_AT_MOST = 64;
    std::uint32_t releasesQueued = 0;

    void ReleaseOnRenderThread(void* object)
    {
        typedef void(__cdecl * R_ReleaseOnRenderThread_t)(void* object);
        typedef void(__cdecl * R_WaitForRenderThreadQueue_t)();
        reinterpret_cast<R_ReleaseOnRenderThread_t>(GetGameAddresses().R_ReleaseOnRenderThread())(object);
        if (++releasesQueued < RELEASES_QUEUED_AT_MOST)
            return;
        reinterpret_cast<R_WaitForRenderThreadQueue_t>(GetGameAddresses().R_WaitForRenderThreadQueue())();
        releasesQueued = 0;
    }

    // A model's unload handler releases its surfaces' buffers right away on the thread unloading it (0x6F2310): they
    // are released on the render thread first (see ReleaseOnRenderThread), and the handler finds none.
    void ReleaseModelBuffersOnRenderThread(XModel& model)
    {
        for (std::uint32_t i = 0; model.surfs && i < model.numsurfs; ++i)
        {
            for (const auto buffer : {&model.surfs[i].vertexBuffer, &model.surfs[i].indexBuffer})
            {
                if (!*buffer)
                    continue;
                ReleaseOnRenderThread(*buffer);
                *buffer = nullptr;
            }
        }
    }

    // releases the Direct3D objects a world's header points to (its vertex and index buffers)
    void ReleaseWorldBuffers(const void* world)
    {
        typedef std::int32_t(__cdecl * DB_GetXAssetTypeSize_t)(std::int32_t type);
        const auto size =
            reinterpret_cast<DB_GetXAssetTypeSize_t>(GetGameAddresses().DB_GetXAssetTypeSize())(ASSET_TYPE_GFXWORLD);
        const auto modules = GetDirect3DModules();
        const auto inModule = [&](std::uintptr_t address) {
            return std::ranges::any_of(modules,
                                       [&](const auto& m) { return address >= m.first && address < m.second; });
        };

        std::unordered_set<IUnknown*> objects;
        const auto fields = static_cast<void* const*>(world);
        for (std::int32_t i = 0; i < size / 4; ++i)
        {
            const auto object = fields[i];
            if (!IsReadable(object, sizeof(void*)))
                continue;
            const auto vtable = *static_cast<void* const*>(object);
            if (!inModule(reinterpret_cast<std::uintptr_t>(vtable)) || !IsReadable(vtable, sizeof(void*)) ||
                !inModule(*static_cast<const std::uintptr_t*>(vtable)))
                continue;
            objects.insert(static_cast<IUnknown*>(object));
        }
        for (const auto object : objects)
        {
            ReleaseOnRenderThread(object);
            ++result->releasedWorldBuffers;
        }
    }

    inline constexpr std::int32_t ASSET_TYPE_SOUND = 9;
    inline constexpr std::int32_t ASSET_TYPE_SOUND_PATCH = 10;
    inline constexpr std::int32_t CRITSECT_SOUND = 8;
    std::unordered_set<std::string> keepModels;
    std::array<std::unordered_set<std::string>, ASSET_TYPE_COUNT> keepByName;

    bool IsCharacterModel(std::string_view name)
    {
        return name.starts_with("c_");
    }

    const char* GetAssetName(std::int32_t type, void* header)
    {
        typedef const char*(__cdecl * DB_GetXAssetName_t)(const XAsset* asset);
        const XAsset asset{type, header};
        const auto name = reinterpret_cast<DB_GetXAssetName_t>(GetGameAddresses().DB_GetXAssetName())(&asset);
        return name ? name : "";
    }

    const PackZone* GetLoadingPackZone()
    {
        if (pass == Pass::None)
            return nullptr;
        const auto index = *reinterpret_cast<std::int32_t*>(GetGameAddresses().g_zoneIndex());
        const auto name = reinterpret_cast<const char*>(PoolGrowth::GetZoneInfo() + index * ZONE_INFO_SIZE);
        const auto zone =
            std::ranges::find_if(packZones, [&](const auto& z) { return _stricmp(z.name.c_str(), name) == 0; });
        return zone != packZones.end() ? &*zone : nullptr;
    }

    bool PoolHasRoom(std::int32_t type)
    {
        const auto pools = reinterpret_cast<void**>(GetGameAddresses().DB_XAssetPool());
        return pools[type] && *static_cast<void**>(pools[type]);
    }

    std::unique_ptr<std::uint8_t[]> CopyHeader(std::int32_t type, const void* header)
    {
        typedef std::int32_t(__cdecl * DB_GetXAssetTypeSize_t)(std::int32_t type);
        const auto size = reinterpret_cast<DB_GetXAssetTypeSize_t>(GetGameAddresses().DB_GetXAssetTypeSize())(type);
        std::unique_ptr<std::uint8_t[]> copy(new std::uint8_t[size]);
        std::memcpy(copy.get(), header, size);
        return copy;
    }

    void* KeepHeader(std::int32_t type, void* header)
    {
        auto& copy = headerCopies.emplace_back(CopyHeader(type, header));
        copies.insert(copy.get());
        if (type == ASSET_TYPE_SOUND || type == ASSET_TYPE_SOUND_PATCH)
            soundCopies.push_back(copy.get());
        if (HasResources(type))
            unloadableCopies.emplace_back(type, copy.get());
        return copy.get();
    }

    void RemoveFromList(void** entries, std::int32_t& count)
    {
        std::int32_t kept = 0;
        for (std::int32_t i = 0; i < count; ++i)
            if (std::ranges::find(soundCopies, entries[i]) == soundCopies.end())
                entries[kept++] = entries[i];
        for (std::int32_t i = kept; i < count; ++i)
            entries[i] = nullptr;
        count = kept;
    }

    // the vertex buffers, shaders and textures of what was left out, which nothing draws with
    void ReleaseCopies()
    {
        typedef void(__cdecl * DB_XAssetUnloadHandler_t)(void* header);
        const auto handlers = reinterpret_cast<DB_XAssetUnloadHandler_t*>(GetGameAddresses().DB_XAssetUnloadHandlers());
        for (const auto& [type, header] : unloadableCopies)
        {
            if (type == ASSET_TYPE_GFXWORLD)
            {
                ReleaseWorldBuffers(header);
                continue;
            }
            if (type == ASSET_TYPE_XMODEL)
                ReleaseModelBuffersOnRenderThread(*static_cast<XModel*>(header));
            if (type == ASSET_TYPE_IMAGE)
            {
                auto& image = *static_cast<GfxImage*>(header);
                if (image.texture)
                {
                    ++result->releasedTextures;
                    result->releasedTextureBytes += image.cardMemory;
                }
                // The handler takes a streamed image out of the streaming system's bit arrays by its index in the
                // image pool, which a copy isn't in: with a copy's address the bits it cleared were far outside them,
                // in whatever memory lay there, and the game crashed or hung in the driver later, now and then (most
                // images of levels and characters are streamed). A copy was never added, so never streamed in.
                image.streaming = false;
            }
            if (handlers[type])
                handlers[type](header);
        }
        unloadableCopies.clear();
    }

    // What a character zone has that another zone registered by that name, left out for the registered one (see
    // AddCharacterZoneAsset), still got its Direct3D objects as it loaded: an image its texture, a technique set its
    // shaders and vertex declarations, a model its surfaces' buffers. Unloading the zone releases only what it
    // registered, so they were left to the process: its heap grew 7 MB every time three characters came and went (67
    // textures and about 900 shaders and declarations). They are released after the load (ReleaseDuplicates), apart
    // from the shaders and vertex declarations that technique sets the zone registered draw with too (see
    // MaterialShader).
    struct Duplicate
    {
        std::int32_t type;
        std::unique_ptr<std::uint8_t[]> header;  // a copy (the loader reads the next asset's into the same block)
        const void* registered;                  // what it was left out for
    };
    std::vector<Duplicate> duplicates;  // of the character zone loading

    // the Direct3D objects of the character zone's duplicates (see Duplicate), released as unloading them would
    // (images: 0x80EDA0, models: 0x6F2310, technique sets: 0x6D7580) but on the render thread
    void ReleaseDuplicates(const CharacterZone& zone)
    {
        // what the zone's own technique sets and models draw with, and the registered ones the duplicates were left
        // out for (from other zones, so they share nothing with them, but nothing of theirs may go)
        std::unordered_set<const void*> kept;
        const auto keep = [&](const MaterialTechniqueSet& techniqueSet) {
            ForEachPass(techniqueSet, [&](const MaterialPass& pass) {
                kept.insert({pass.vertexDecl, pass.vertexShader, pass.pixelShader});
            });
        };
        for (const auto techniqueSet : zone.techniqueSets)
            keep(*techniqueSet);
        for (const auto model : packModels)
            kept.insert(model->surfs);
        for (const auto& duplicate : duplicates)
        {
            if (duplicate.type == ASSET_TYPE_TECHNIQUE_SET)
                keep(*static_cast<const MaterialTechniqueSet*>(duplicate.registered));
            else if (duplicate.type == ASSET_TYPE_XMODEL)
                kept.insert(static_cast<const XModel*>(duplicate.registered)->surfs);
            else if (duplicate.type == ASSET_TYPE_IMAGE)
                kept.insert(static_cast<const GfxImage*>(duplicate.registered)->texture);
        }

        typedef void(__cdecl * DB_XAssetUnloadHandler_t)(void* header);
        const auto handlers = reinterpret_cast<DB_XAssetUnloadHandler_t*>(GetGameAddresses().DB_XAssetUnloadHandlers());
        std::unordered_set<const void*> released;
        const auto release = [](void*& object) {
            if (!object)
                return;
            ReleaseOnRenderThread(object);
            object = nullptr;
            ++result->releasedShaders;
        };
        for (const auto& [type, header, registered] : duplicates)
        {
            if (type == ASSET_TYPE_IMAGE)
            {
                auto& image = *reinterpret_cast<GfxImage*>(header.get());
                if (kept.contains(image.texture))
                    continue;
                if (image.texture)
                {
                    ++result->releasedTextures;
                    result->releasedTextureBytes += image.cardMemory;
                }
                // (never added, so never streamed in: see ReleaseCopies)
                image.streaming = false;
                if (handlers[type])
                    handlers[type](&image);
            }
            else if (type == ASSET_TYPE_TECHNIQUE_SET)
            {
                ForEachPass(*reinterpret_cast<const MaterialTechniqueSet*>(header.get()),
                            [&](const MaterialPass& pass) {
                                for (const auto shader : {pass.vertexShader, pass.pixelShader})
                                {
                                    if (!shader || !released.insert(shader).second)
                                        continue;
                                    if (kept.contains(shader))
                                        result->keptShaders += shader->program ? 1 : 0;
                                    else
                                        release(shader->program);
                                }
                                const auto declaration = pass.vertexDecl;
                                if (!declaration || !released.insert(declaration).second || !declaration->isLoaded)
                                    return;
                                if (kept.contains(declaration))
                                {
                                    ++result->keptShaders;
                                    return;
                                }
                                declaration->isLoaded = false;
                                for (std::size_t i = 0; i < RELEASED_DECLARATION_COUNT; ++i)
                                    release(declaration->declarations[i]);
                            });
            }
            else if (type == ASSET_TYPE_XMODEL)
            {
                auto& model = *reinterpret_cast<XModel*>(header.get());
                if (!kept.contains(model.surfs))
                    ReleaseModelBuffersOnRenderThread(model);
            }
        }
        duplicates.clear();
    }

    void RemoveSoundCopies()
    {
        if (soundCopies.empty())
            return;

        typedef void(__cdecl * Sys_CriticalSection_t)(std::int32_t section);
        const auto& addresses = GetGameAddresses();
        reinterpret_cast<Sys_CriticalSection_t>(addresses.Sys_EnterCriticalSection())(CRITSECT_SOUND);
        RemoveFromList(reinterpret_cast<void**>(addresses.snd_banks()),
                       *reinterpret_cast<std::int32_t*>(addresses.snd_bankCount()));
        RemoveFromList(reinterpret_cast<void**>(addresses.snd_patches()),
                       *reinterpret_cast<std::int32_t*>(addresses.snd_patchCount()));
        reinterpret_cast<Sys_CriticalSection_t>(addresses.Sys_LeaveCriticalSection())(CRITSECT_SOUND);
        soundCopies.clear();
    }

    // An unloaded zone's models, materials, technique sets and images stay registered by name as stand-ins with the
    // data of the type's default asset ("void", "$default", "default", "$white"), in zone 0, when they are marked in
    // use (see PrepareUnloadOf; the rest are freed after the unload); a zone loaded later with an asset of that name
    // fills its stand-in again. So a stand-in doesn't count as registered.
    inline constexpr std::uint8_t STAND_IN_ZONE = 0;

    // the entry of the registered asset of a type and name, if any (not a stand-in)
    const XAssetEntry* FindRegisteredEntry(std::int32_t type, const char* name)
    {
        typedef std::uint32_t(__cdecl * DB_HashForName_t)(const char* name, std::int32_t type);
        const auto& addresses = GetGameAddresses();
        const auto hash = reinterpret_cast<DB_HashForName_t>(addresses.DB_HashForName())(name, type) & 0x7FFF;
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(addresses.db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(addresses.g_assetEntryPool());
        for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
        {
            const auto& entry = entries[index];
            if (entry.type == type && entry.header && entry.zoneIndex != STAND_IN_ZONE &&
                _stricmp(GetAssetName(type, const_cast<void*>(entry.header)), name) == 0)
                return &entry;
        }
        return nullptr;
    }

    // the registered asset of a type and name, if any
    void* FindRegistered(std::int32_t type, const char* name)
    {
        const auto entry = FindRegisteredEntry(type, name);
        return entry ? const_cast<void*>(entry->header) : nullptr;
    }

    std::uintptr_t DB_AddXAsset_Trampoline;

    void* AddXAsset(std::int32_t assetType, void* assetHeader)
    {
        void* added;
        __asm
        {
            mov eax, assetType
            push assetHeader
            call DB_AddXAsset_Trampoline
            add esp, 4
            mov added, eax
        }
        return added;
    }

    // What a kept asset draws with has to be registered too, since the renderer finds images by their place in the
    // pool and sorts the materials of the pool. Whatever the survey missed is registered from its copy as the asset
    // using it is.
    template <typename T>
    void RegisterCopy(std::int32_t type, T*& asset)
    {
        if (!asset || !copies.contains(asset))
            return;
        if (const auto registered = FindRegistered(type, GetAssetName(type, asset)))
        {
            asset = static_cast<T*>(registered);
            ++result->shared[type];
            return;
        }
        if (!PoolHasRoom(type))
            return;
        // its resources are the registered asset's now
        std::erase(unloadableCopies, std::pair<std::int32_t, void*>(type, asset));
        asset = static_cast<T*>(AddXAsset(type, asset));
        if constexpr (std::is_same_v<T, MaterialTechniqueSet>)
            packTechniqueSets.insert(asset);
        ++result->kept[type];
        ++result->registeredLate;
    }

    void RegisterTextures(Material& material)
    {
        RegisterCopy(ASSET_TYPE_TECHNIQUE_SET, material.techniqueSet);
        for (std::uint32_t texture = 0; material.textureTable && texture < material.textureCount; ++texture)
            RegisterCopy(ASSET_TYPE_IMAGE, material.textureTable[texture].image);
    }

    void RegisterMaterials(XModel& model)
    {
        for (std::uint32_t surface = 0; model.materialHandles && surface < model.numsurfs; ++surface)
        {
            auto& material = model.materialHandles[surface];
            if (material && copies.contains(material))
                RegisterTextures(*material);
            RegisterCopy(ASSET_TYPE_MATERIAL, material);
        }
    }

    std::string packZone;  // the zone of the pack loaded, empty for none

    // Everything a character zone has is wanted: what isn't registered by that name yet is registered. What is (by
    // multiplayer, the pack or another character zone) is used instead, as packs do, which ties the zone to the one
    // that registered it (see UnloadCharacterZone).
    void* AddCharacterZoneAsset(std::int32_t type, void* header)
    {
        auto& zone = *loadingCharacterZone;
        const std::string name = GetAssetName(type, header);
        if (const auto registered = FindRegisteredEntry(type, name.c_str()))
        {
            ++result->shared[type];
            const auto owner = GetZoneName(registered->zoneIndex);
            if (FindCharacterZone(owner))
                zone.dependsOn.emplace(owner);
            else if (!packZone.empty() && _stricmp(std::string(owner).c_str(), packZone.c_str()) == 0)
                zone.usesPack = true;
            if (type == ASSET_TYPE_XMODEL)
                result->models.push_back(name);
            if (HasResources(type))
                duplicates.push_back({type, CopyHeader(type, header), registered->header});
            return const_cast<void*>(registered->header);
        }
        // DB_AllocXAssetHeader ends the game when a pool is full
        if (!PoolHasRoom(type))
        {
            ++result->poolFull[type];
            return KeepHeader(type, header);
        }

        ++result->kept[type];
        const auto added = AddXAsset(type, header);
        if (type == ASSET_TYPE_XMODEL)
        {
            result->models.push_back(name);
            packModels.push_back(static_cast<XModel*>(added));
        }
        else if (type == ASSET_TYPE_TECHNIQUE_SET)
        {
            zone.techniqueSets.insert(static_cast<MaterialTechniqueSet*>(added));
        }
        else if (type == ASSET_TYPE_IMAGE)
        {
            zone.images.push_back(static_cast<GfxImage*>(added));
        }
        else if (type == ASSET_TYPE_WEAPON)
        {
            zone.weapons.push_back(added);
        }
        else if (type == ASSET_TYPE_SOUND)
        {
            zone.soundBank = added;
        }
        return added;
    }

    void* __cdecl OnAddXAsset(std::int32_t type, void* header)
    {
        const auto zone = GetLoadingPackZone();
        if (!zone && pass == Pass::None)
            CampaignMaps::FixClientScript(type, header);
        // a campaign level's zone, loaded as a multiplayer map, or its players' kit (see CampaignMaps)
        if (!zone && pass == Pass::None && type >= 0 && type < ASSET_TYPE_COUNT)
            if (const auto handled = CampaignMaps::AddAsset(type, header))
                return *handled;
        if (!zone || type < 0 || type >= ASSET_TYPE_COUNT)
            return AddXAsset(type, header);
        if (zone->keep == Keep::All)
            return AddCharacterZoneAsset(type, header);

        if (pass == Pass::Survey)
        {
            const auto copy = KeepHeader(type, header);
            if (zone->keep == Keep::Characters && type == ASSET_TYPE_XMODEL &&
                IsCharacterModel(GetAssetName(type, copy)))
                surveyedModels.push_back(static_cast<XModel*>(copy));
            ++result->skipped[type];
            return copy;
        }

        const std::string name = GetAssetName(type, header);
        const bool keep = zone->keep == Keep::Animations ? type == ASSET_TYPE_XANIMPARTS
                          : type == ASSET_TYPE_XMODEL    ? keepModels.contains(name)
                                                         : keepByName[type].contains(name);
        if (!keep)
        {
            ++result->skipped[type];
            return KeepHeader(type, header);
        }
        if (const auto registered = FindRegistered(type, name.c_str()))
        {
            ++result->shared[type];
            if (type == ASSET_TYPE_XMODEL)
                result->models.push_back(name);
            return registered;
        }
        // DB_AllocXAssetHeader ends the game when a pool is full
        if (!PoolHasRoom(type))
        {
            ++result->poolFull[type];
            return KeepHeader(type, header);
        }

        ++result->kept[type];
        if (type == ASSET_TYPE_XMODEL)
        {
            result->models.push_back(name);
            RegisterMaterials(*static_cast<XModel*>(header));
        }
        else if (type == ASSET_TYPE_MATERIAL)
        {
            RegisterTextures(*static_cast<Material*>(header));
        }
        const auto added = AddXAsset(type, header);
        if (type == ASSET_TYPE_XMODEL)
            packModels.push_back(static_cast<XModel*>(added));
        else if (type == ASSET_TYPE_TECHNIQUE_SET)
            packTechniqueSets.insert(static_cast<MaterialTechniqueSet*>(added));
        return added;
    }

    // usercall: the type in eax, the header on the stack (the caller removes it)
    void __declspec(naked) DB_AddXAsset_Hook()
    {
        __asm
        {
            push [esp + 4]
            push eax
            call OnAddXAsset
            add esp, 8
            ret
        }
    }

    bool IsReadable(const void* address, std::size_t size)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        const auto end = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize;
        return static_cast<const std::uint8_t*>(address) + size <= end;
    }

    // a name the walk found, or nothing when it points at memory that isn't one
    std::optional<std::string> ReadName(const char* name)
    {
        if (!IsReadable(name, 1))
            return std::nullopt;
        const auto length = strnlen(name, 128);
        if (length == 0 || length == 128 || !IsReadable(name, length + 1))
            return std::nullopt;
        for (std::size_t i = 0; i < length; ++i)
            if (name[i] < 0x20 || name[i] > 0x7E)
                return std::nullopt;
        return std::string(name, length);
    }

    void Keep(std::int32_t type, const char* name)
    {
        if (const auto valid = ReadName(name))
            keepByName[type].insert(*valid);
        else
            ++result->invalidNames;
    }

    void CollectCharacterAssets()
    {
        for (const auto model : surveyedModels)
        {
            keepModels.insert(model->name);
            if (!IsReadable(model->materialHandles, model->numsurfs * sizeof(Material*)))
            {
                ++result->invalidNames;
                continue;
            }

            for (std::uint32_t surface = 0; surface < model->numsurfs; ++surface)
            {
                const auto material = model->materialHandles[surface];
                if (!IsReadable(material, sizeof(Material)))
                {
                    ++result->invalidNames;
                    continue;
                }

                Keep(ASSET_TYPE_MATERIAL, material->name);
                if (IsReadable(material->techniqueSet, sizeof(MaterialTechniqueSet)))
                    Keep(ASSET_TYPE_TECHNIQUE_SET, material->techniqueSet->name);
                if (!IsReadable(material->textureTable, material->textureCount * sizeof(MaterialTextureDef)))
                    continue;
                for (std::uint32_t texture = 0; texture < material->textureCount; ++texture)
                {
                    const auto image = material->textureTable[texture].image;
                    if (IsReadable(image, sizeof(GfxImage)) && ReadName(image->name))
                        Keep(ASSET_TYPE_IMAGE, image->name);
                    else
                    {
                        ++result->invalidNames;
                        const auto raw = reinterpret_cast<const std::uint32_t*>(&material->textureTable[texture]);
                        result->notes.push_back(std::format("{} texture {}/{}: {:08x} {:08x} {:08x} {:08x}",
                                                            ReadName(material->name).value_or("?"), texture,
                                                            material->textureCount, raw[0], raw[1], raw[2], raw[3]));
                    }
                }
            }
        }
    }

    // the vertex format the client draws models with
    inline constexpr std::size_t MODEL_VERTEX_TYPE = 1;

    bool HasModelVertexType(const MaterialTechnique& technique)
    {
        for (std::uint32_t pass = 0; pass < technique.passCount; ++pass)
        {
            const auto vertexDecl = technique.passArray[pass].vertexDecl;
            if (vertexDecl && !vertexDecl->declarations[MODEL_VERTEX_TYPE])
                return false;
        }
        return true;
    }

    // The techniques (by their place in a technique set) that draw models: all but those that some model technique
    // set (named mc_*) of multiplayer can't draw models with, which the renderer evidently doesn't use for models.
    using TechniqueMask = std::bitset<TECHNIQUE_COUNT>;
    TechniqueMask GetModelTechniques()
    {
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        TechniqueMask modelTechniques;
        modelTechniques.set();
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
        {
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
            {
                const auto& entry = entries[index];
                const auto techniqueSet = static_cast<const MaterialTechniqueSet*>(entry.header);
                if (entry.type != ASSET_TYPE_TECHNIQUE_SET || !techniqueSet || entry.zoneIndex == STAND_IN_ZONE ||
                    IsOwnTechniqueSet(techniqueSet) || !std::string_view(techniqueSet->name).starts_with("mc_"))
                    continue;
                for (std::size_t i = 0; i < TECHNIQUE_COUNT; ++i)
                    if (techniqueSet->techniques[i] && !HasModelVertexType(*techniqueSet->techniques[i]))
                        modelTechniques.reset(i);
            }
        }
        return modelTechniques;
    }

    TechniqueMask GetUndrawableTechniques(const MaterialTechniqueSet& techniqueSet,
                                          const TechniqueMask& modelTechniques)
    {
        TechniqueMask undrawable;
        for (std::size_t i = 0; i < TECHNIQUE_COUNT; ++i)
            if (modelTechniques[i] && techniqueSet.techniques[i] && !HasModelVertexType(*techniqueSet.techniques[i]))
                undrawable.set(i);
        return undrawable;
    }

    // what the passes of a technique set look up in a material by name hash: constants and samplers
    struct MaterialArguments
    {
        std::unordered_set<std::uint32_t> constants;
        std::unordered_set<std::uint32_t> samplers;
    };

    MaterialArguments GetMaterialArguments(const MaterialTechniqueSet& techniqueSet)
    {
        MaterialArguments arguments;
        for (const auto technique : techniqueSet.techniques)
        {
            for (std::uint32_t pass = 0; technique && pass < technique->passCount; ++pass)
            {
                const auto& materialPass = technique->passArray[pass];
                const auto count = materialPass.perPrimArgCount + materialPass.perObjArgCount +
                                   materialPass.stableArgCount;
                for (std::int32_t i = 0; materialPass.args && i < count; ++i)
                {
                    const auto& argument = materialPass.args[i];
                    if (argument.type == MTL_ARG_MATERIAL_VERTEX_CONST || argument.type == MTL_ARG_MATERIAL_PIXEL_CONST)
                        arguments.constants.insert(argument.nameHash);
                    else if (argument.type == MTL_ARG_MATERIAL_PIXEL_SAMPLER)
                        arguments.samplers.insert(argument.nameHash);
                }
            }
        }
        return arguments;
    }

    bool HasConstant(const Material& material, std::uint32_t nameHash)
    {
        for (std::uint32_t i = 0; material.constantTable && i < material.constantCount; ++i)
            if (material.constantTable[i].nameHash == nameHash)
                return true;
        return false;
    }

    bool HasSampler(const Material& material, std::uint32_t nameHash)
    {
        for (std::uint32_t i = 0; material.textureTable && i < material.textureCount; ++i)
            if (material.textureTable[i].nameHash == nameHash)
                return true;
        return false;
    }

    // what the material lacks of the arguments
    MaterialArguments GetMissingArguments(const Material& material, const MaterialArguments& arguments)
    {
        MaterialArguments missing;
        for (const auto nameHash : arguments.constants)
            if (!HasConstant(material, nameHash))
                missing.constants.insert(nameHash);
        for (const auto nameHash : arguments.samplers)
            if (!HasSampler(material, nameHash))
                missing.samplers.insert(nameHash);
        return missing;
    }

    // a registered material drawn with the technique set (multiplayer's, or one of the pack's that has them already)
    // which has all of the arguments; not one of a character zone, which may be unloaded before the material it gives
    // them to (see UnloadCharacterZone), unless it is in the same zone (the one loading), nor for a character zone one
    // of the pack
    const Material* FindMaterialWith(const MaterialTechniqueSet* techniqueSet, const MaterialArguments& arguments)
    {
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        const auto otherZone = [&](const XAssetEntry& entry) {
            const auto zone = GetZoneName(entry.zoneIndex);
            return entry.zoneIndex == STAND_IN_ZONE || FindCharacterZone(zone) ||
                   (loadingCharacterZone && !packZone.empty() &&
                    _stricmp(std::string(zone).c_str(), packZone.c_str()) == 0);
        };
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
        {
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
            {
                const auto& entry = entries[index];
                const auto material = static_cast<const Material*>(entry.header);
                if (entry.type != ASSET_TYPE_MATERIAL || !material || material->techniqueSet != techniqueSet ||
                    otherZone(entry))
                    continue;
                if (const auto missing = GetMissingArguments(*material, arguments);
                    missing.constants.empty() && missing.samplers.empty())
                    return material;
            }
        }
        return nullptr;
    }

    // the constant and texture tables made for the pack's materials, which go with its zone
    std::vector<std::unique_ptr<MaterialConstantDef[]>> constantTables;
    std::vector<std::unique_ptr<MaterialTextureDef[]>> textureTables;

    // The table with the entries of from whose name hash is missing added, in the order of their name hashes like
    // every table of the game, which the renderer relies on (see MakeDrawable)
    template <typename T>
    std::span<T> AddEntries(std::vector<std::unique_ptr<T[]>>& tables, std::span<const T> table,
                            std::span<const T> from, std::unordered_set<std::uint32_t> missing)
    {
        auto& added = tables.emplace_back(new T[table.size() + missing.size()]);
        auto count = std::ranges::copy(table, added.get()).out - added.get();
        for (const auto& entry : from)
            if (missing.erase(entry.nameHash))
                added[count++] = entry;
        const std::span<T> result(added.get(), count);
        std::ranges::sort(result, {}, &T::nameHash);
        return result;
    }

    // Gives the material what it lacks of the arguments, from a material that has them
    void AddArguments(Material& material, const MaterialArguments& missing, const Material& from)
    {
        if (!missing.constants.empty())
        {
            const auto table = AddEntries<MaterialConstantDef>(
                constantTables, {material.constantTable, material.constantCount},
                {from.constantTable, from.constantCount}, missing.constants);
            material.constantTable = table.data();
            material.constantCount = static_cast<std::uint8_t>(table.size());
        }
        if (!missing.samplers.empty())
        {
            const auto table = AddEntries<MaterialTextureDef>(
                textureTables, {material.textureTable, material.textureCount}, {from.textureTable, from.textureCount},
                missing.samplers);
            material.textureTable = table.data();
            material.textureCount = static_cast<std::uint8_t>(table.size());
        }
    }

    std::string GetConstantNames(const Material& material, const std::unordered_set<std::uint32_t>& nameHashes)
    {
        std::string names;
        for (std::uint32_t i = 0; i < material.constantCount; ++i)
        {
            const auto& constant = material.constantTable[i];
            if (nameHashes.contains(constant.nameHash))
                names += (names.empty() ? "" : ",") +
                         std::string(constant.name, strnlen(constant.name, sizeof(constant.name)));
        }
        return names;
    }

    // Single player draws some characters with vertex formats multiplayer doesn't have, e.g. for the skin tension of
    // faces, and the technique sets made for them have no vertex declaration for the format models are drawn with
    // here, which stops the game with "Vertex type 1 doesn't have the information used by shader". Such a material
    // gets the multiplayer technique set its name comes from, without the last parts that name the single player
    // feature ("mc_sw4_3d_char_skin_tension" gets "mc_sw4_3d_char_skin", "mc_l_sm_b0c0n0_hero" gets "mc_l_sm_b0c0n0"),
    // which has the same techniques. Without one, the techniques that can't draw models are taken out, which leaves the
    // surface out of what they draw.
    //
    // The renderer looks up the constants and samplers a pass asks for in the material by name hash, going through
    // the pass's arguments and the material's table together (both in the order of their name hashes), and doesn't
    // stop until it finds each, so a material missing one that its new technique set's shaders read ran off its table
    // and crashed the game as soon as the materials were sorted again (the frame after loading a pack). Such a
    // material gets the missing ones from another material drawn with that technique set (their values, and the
    // image for a sampler), in their place in the table; without one, it keeps its technique set and loses the
    // techniques instead.
    void MakeDrawable(LoadResult& loadResult)
    {
        const auto modelTechniques = GetModelTechniques();
        for (const auto& name : loadResult.models)
        {
            const auto model = static_cast<XModel*>(FindRegistered(ASSET_TYPE_XMODEL, name.c_str()));
            for (std::uint32_t surface = 0; model && model->materialHandles && surface < model->numsurfs; ++surface)
            {
                const auto material = model->materialHandles[surface];
                if (!material || !IsOwnTechniqueSet(material->techniqueSet))
                    continue;
                const auto undrawable = GetUndrawableTechniques(*material->techniqueSet, modelTechniques);
                if (undrawable.none())
                    continue;

                MaterialTechniqueSet* plain = nullptr;
                std::string plainName = material->techniqueSet->name;
                while (!plain && std::ranges::count(plainName, '_') > 2)
                {
                    plainName.erase(plainName.rfind('_'));
                    plain =
                        static_cast<MaterialTechniqueSet*>(FindRegistered(ASSET_TYPE_TECHNIQUE_SET, plainName.c_str()));
                    if (plain && IsOwnTechniqueSet(plain))
                        plain = nullptr;
                }
                if (plain)
                {
                    const auto arguments = GetMaterialArguments(*plain);
                    const auto missing = GetMissingArguments(*material, arguments);
                    const auto from = missing.constants.empty() && missing.samplers.empty()
                                          ? material
                                          : FindMaterialWith(plain, arguments);
                    if (from && from != material)
                    {
                        AddArguments(*material, missing, *from);
                        loadResult.notes.push_back(std::format(
                            "{} ({}): took constants {} and {} samplers from {}", material->name, plain->name,
                            GetConstantNames(*from, missing.constants), missing.samplers.size(), from->name));
                        loadResult.addedArguments +=
                            static_cast<std::uint32_t>(missing.constants.size() + missing.samplers.size());
                    }
                    if (from)
                    {
                        material->techniqueSet = plain;
                        ++loadResult.replacedTechniqueSets;
                        continue;
                    }
                    loadResult.notes.push_back(std::format("{} ({}): no material has the {} constants and {} "
                                                           "samplers it lacks",
                                                           material->name, plain->name, missing.constants.size(),
                                                           missing.samplers.size()));
                }

                for (std::size_t i = 0; i < TECHNIQUE_COUNT; ++i)
                    if (undrawable[i])
                        material->techniqueSet->techniques[i] = nullptr;
                loadResult.notes.push_back(std::format("{} ({}): took out {} techniques", material->name,
                                                       material->techniqueSet->name, undrawable.count()));
                loadResult.removedTechniques += static_cast<std::uint32_t>(undrawable.count());
            }
        }
    }

    // waits until the render thread has drawn what it was handed (on the main thread; it returns at once elsewhere)
    void SyncRenderThread()
    {
        typedef int(__cdecl * R_SyncRenderThread_t)();
        reinterpret_cast<R_SyncRenderThread_t>(GetGameAddresses().R_SyncRenderThread())();
    }

    // Releases the actors before zones whose models they may be made of are unloaded. The render thread draws the
    // frame the main thread built before, which may still have them, and their materials, techniques and surfaces are
    // in the zones' memory, which the unload frees at once: so it is waited for. Freed under it as it drew them (an
    // actor clear and an unload in the next frame, as the zones' budget does), the render thread hung in the driver.
    void ReleaseActorsForUnload()
    {
        Actors::ReleaseAll();
        SyncRenderThread();
    }

    // The renderer gives the models their index buffers (and the vertex buffers of rigid surfaces) once a map has
    // loaded, for all models there are then, so a pack's models get theirs here. Drawn without, they come out as
    // spikes.
    void CreateModelBuffers()
    {
        typedef void(__cdecl * R_CreateXModelBuffers_t)(XModel * model);
        const auto R_CreateXModelBuffers =
            reinterpret_cast<R_CreateXModelBuffers_t>(GetGameAddresses().R_CreateXModelBuffers());
        // made while the render thread waits, as it draws with the same device
        if (!packModels.empty())
            SyncRenderThread();
        for (const auto model : packModels)
            R_CreateXModelBuffers(model);
        packModels.clear();
    }

    // loads and unloads zones (DB_LoadXAssets) the way the loadzone command does
    // With refresh, what the client takes from the zones is refreshed after, as the loadzone command does; not while
    // the client starts (a film's weapons, see WeaponZones), whose start does that itself after, and whose state the
    // refresh read before it was set up (the game crashed at 0x5D0707)
    void LoadXAssets(std::vector<XZoneInfo>& zones, bool refresh = true)
    {
        typedef void(__cdecl * Function_t)();
        typedef bool(__cdecl * Check_t)();
        typedef void(__cdecl * DB_LoadXAssets_t)(XZoneInfo * zones, std::uint32_t count, std::int32_t sync);
        const auto& addresses = GetGameAddresses();
        reinterpret_cast<Function_t>(addresses.DB_BeforeLoadZones())();
        reinterpret_cast<DB_LoadXAssets_t>(addresses.DB_LoadXAssets())(zones.data(),
                                                                       static_cast<std::uint32_t>(zones.size()), 1);
        if (!refresh)
            return;
        reinterpret_cast<Function_t>(addresses.DB_AfterLoadZones1())();
        if (!reinterpret_cast<Check_t>(addresses.DB_AfterLoadZones2())())
            reinterpret_cast<Function_t>(addresses.DB_AfterLoadZones3())();
        reinterpret_cast<Function_t>(addresses.DB_AfterLoadZones4())();
    }

    // loads the pack's zones (none to unload them) the way the loadzone command loads one, which unloads the zones
    // loaded with the same flags before
    void LoadZones(std::int32_t flags)
    {
        std::vector<XZoneInfo> zones{{nullptr, 0, flags}};
        for (const auto& zone : packZones)
            zones.push_back({zone.name.c_str(), flags, 0});
        LoadXAssets(zones);
    }

    std::int64_t GetPrivateBytes()
    {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters));
        return static_cast<std::int64_t>(counters.PrivateUsage);
    }

    bool IsZoneLoaded(std::string_view name);

    void LoadAnimations()
    {
        // A map change keeps the zone (no zone of the game has its flag): its animations are there still. Loading it
        // again would unload it first, and the animations of it that were in use would stay stand-ins (see
        // STAND_IN_ZONE) until the game fills them in from the new copy, after the next load.
        if (IsZoneLoaded(ANIMATIONS_ZONE))
        {
            animationsLoaded = true;
            return;
        }
        packZones = {{std::string(ANIMATIONS_ZONE), Keep::Animations}};
        pass = Pass::Load;
        LoadZones(ANIMATIONS_ZONE_FLAGS);
        pass = Pass::None;
        RemoveSoundCopies();
        ReleaseCopies();
        packZones.clear();
        animationsLoaded = true;
    }

    LoadResult Load(const std::string& zone, bool animations)
    {
        LoadResult loadResult;
        result = &loadResult;
        loadResult.memoryBefore = GetPrivateBytes();
        // (with the animation pack, actors play the campaign's animations from the store instead)
        if (((animations && !AnimationStore::IsReady()) || zone == ANIMATIONS_ZONE) && !animationsLoaded)
            LoadAnimations();
        if (zone.empty() || zone == ANIMATIONS_ZONE)
        {
            loadResult.loaded = true;
            loadResult.memoryAfter = GetPrivateBytes();
            result = nullptr;
            return loadResult;
        }

        packZones = {{zone, Keep::Characters}};
        packZone.clear();
        surveyedModels.clear();
        packModels.clear();
        packTechniqueSets.clear();
        keepModels.clear();
        for (auto& names : keepByName)
            names.clear();

        pass = Pass::Survey;
        LoadZones(PACK_ZONE_FLAGS);
        pass = Pass::None;
        RemoveSoundCopies();
        ReleaseCopies();
        // the pack before has gone with its zone
        constantTables.clear();
        textureTables.clear();
        loadResult.memorySurveyed = GetPrivateBytes();

        if (surveyedModels.empty())
        {
            loadResult.error = "no character models in " + zone;
            packZones.clear();
        }

        CollectCharacterAssets();
        surveyedModels.clear();
        loadResult.skipped = {};

        // the survey's copies go with its zone, which loading the zone again unloads
        auto surveyCopies = std::move(headerCopies);
        headerCopies.clear();
        copies.clear();

        pass = Pass::Load;
        LoadZones(PACK_ZONE_FLAGS);
        pass = Pass::None;
        RemoveSoundCopies();
        ReleaseCopies();
        surveyCopies.clear();
        packZones.clear();
        MakeDrawable(loadResult);
        CreateModelBuffers();
        packZone = loadResult.error.empty() ? zone : std::string();
        loadResult.memoryAfter = GetPrivateBytes();
        loadResult.loaded = true;
        result = nullptr;
        return loadResult;
    }

    // the zones the game has loaded: their names and allocation flags (g_zoneInfo, whose first entry is never used)
    inline constexpr std::size_t ZONE_INFO_FLAGS = 0x40;
    std::vector<std::pair<std::string, std::int32_t>> GetLoadedZones()
    {
        std::vector<std::pair<std::string, std::int32_t>> zones;
        for (std::size_t i = 1; i < PoolGrowth::GetZoneInfoCount(); ++i)
        {
            const auto info = PoolGrowth::GetZoneInfo() + i * ZONE_INFO_SIZE;
            const auto name = reinterpret_cast<const char*>(info);
            if (name[0])
                zones.emplace_back(std::string(name, strnlen(name, ZONE_INFO_FLAGS)),
                                   *reinterpret_cast<const std::int32_t*>(info + ZONE_INFO_FLAGS));
        }
        return zones;
    }

    std::uint32_t CountFreeSlots(std::int32_t type)
    {
        // a pool's first member is its next free slot, and each free slot starts with the next
        constexpr std::uint32_t MAX_COUNTED = 100000;
        const auto pools = reinterpret_cast<void**>(GetGameAddresses().DB_XAssetPool());
        if (type < 0 || type >= ASSET_TYPE_COUNT || !pools[type])
            return 0;
        std::uint32_t free = 0;
        for (auto slot = *static_cast<void**>(pools[type]); slot && free < MAX_COUNTED;
             slot = *static_cast<void**>(slot))
            ++free;
        return free;
    }

    std::uint32_t CountRegisteredIn(std::int32_t type, std::uintptr_t begin, std::uintptr_t end)
    {
        std::uint32_t count = 0;
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
        {
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
            {
                const auto header = reinterpret_cast<std::uintptr_t>(entries[index].header);
                if (entries[index].type == type && header >= begin && header < end)
                    ++count;
            }
        }
        return count;
    }

    std::vector<std::string> DescribeEntries(std::int32_t type, const std::string& name)
    {
        std::vector<std::string> described;
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
        {
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
            {
                const auto& entry = entries[index];
                if (entry.type != type || !entry.header ||
                    _stricmp(GetAssetName(type, const_cast<void*>(entry.header)), name.c_str()) != 0)
                    continue;
                described.push_back(std::format("entry {} zone {} '{}' inuse {} override {} header {:08X}", index,
                                                entry.zoneIndex, GetZoneName(entry.zoneIndex), entry.inuse,
                                                entry.nextOverride, reinterpret_cast<std::uintptr_t>(entry.header)));
            }
        }
        return described;
    }

    bool IsZoneLoaded(std::string_view name)
    {
        return std::ranges::any_of(GetLoadedZones(), [&](const auto& zone) {
            return _stricmp(zone.first.c_str(), std::string(name).c_str()) == 0;
        });
    }

    std::mutex characterZonesMutex;  // held by other threads than the main one to read characterZones

    // forgets the character zones the game has unloaded (STOP unloads them, see UnloadZonesWithClient), with the
    // tables made for their materials, which went with them
    void ForgetUnloadedCharacterZones()
    {
        std::lock_guard lock(characterZonesMutex);
        std::erase_if(characterZones, [](const CharacterZone& zone) { return !IsZoneLoaded(zone.name); });
        std::erase_if(soundZones, [](const CharacterZone& zone) { return !IsZoneLoaded(zone.name); });
    }

    // the flag bit the fewest loaded character zones have
    std::int32_t ChooseCharacterZoneBit()
    {
        return *std::ranges::min_element(CHARACTER_ZONE_BITS, {}, [](std::int32_t bit) {
            return std::ranges::count_if(characterZones, [&](const auto& zone) { return (zone.flags & bit) != 0; });
        });
    }

    // character zones are named so (see tools/t5-characters/zones.py), the campaign maps' kits (see CampaignMaps), and
    // the campaign's and Zombies' weapons and the sound banks of their shots (tools/t5-weapons/weapons.py), which load
    // as character zones do
    inline constexpr std::string_view CHARACTER_ZONE_PREFIX = "t5c_";
    inline constexpr std::string_view KIT_ZONE_PREFIX = "t5m_";
    inline constexpr std::string_view WEAPON_ZONE_PREFIX = "t5w_";
    inline constexpr std::string_view SOUND_ZONE_PREFIX = "t5s_";

    // whether a zone is one of the mod's folder that loads as a character zone does
    bool IsModZone(std::string_view name)
    {
        return name.starts_with(CHARACTER_ZONE_PREFIX) || name.starts_with(WEAPON_ZONE_PREFIX) ||
               name.starts_with(SOUND_ZONE_PREFIX);
    }
    // the folder of character zones (Components::Actors::GetCharacterZonesFolder), known before the loader's thread
    // asks for paths (see FormatZonePath)
    std::filesystem::path zonesFolder;

    // where the game finds a zone of that name: a character zone in the mod's folder (see FormatZonePath), else in
    // zone/Common; nothing when it has none
#ifdef IWXMVM_T5_HARNESS
    std::set<std::string> hiddenZoneFiles;
    void HideZoneFiles(std::set<std::string> zones)
    {
        hiddenZoneFiles = std::move(zones);
    }

    // a mod's zone in the folder of IWXMVM_EXTRA_ZONES (launch.ps1 -ExtraZones): zones the setup hasn't built, for a test
    std::optional<std::filesystem::path> FindExtraZoneFile(const std::string& zone)
    {
        char folder[MAX_PATH]{};
        if (!IsModZone(zone) || !GetEnvironmentVariableA("IWXMVM_EXTRA_ZONES", folder, sizeof(folder)))
            return std::nullopt;
        std::error_code error;
        const auto path = std::filesystem::path(folder) / (zone + ".ff");
        return std::filesystem::is_regular_file(path, error) ? std::optional(path) : std::nullopt;
    }
#endif

    std::optional<std::filesystem::path> FindZoneFile(const std::string& zone)
    {
#ifdef IWXMVM_T5_HARNESS
        if (hiddenZoneFiles.contains(zone))
            return std::nullopt;
        if (const auto extra = FindExtraZoneFile(zone))
            return extra;
#endif
        std::error_code error;
        if (IsModZone(zone))
            if (const auto path = zonesFolder / (zone + ".ff"); std::filesystem::is_regular_file(path, error))
                return path;
        const auto basePath = Functions::FindDvar("fs_basepath");
        if (!basePath || !basePath->current.string)
            return std::nullopt;
        const auto path = std::filesystem::path(basePath->current.string) / "zone" / "Common" / (zone + ".ff");
        return std::filesystem::is_regular_file(path, error) ? std::optional(path) : std::nullopt;
    }

    // The loader opens a zone at zone/Common/<name>.ff (unless it is a language's), which it formats with _snprintf;
    // that call comes here instead, and a character zone of the mod's folder gets its path there. On the loader's
    // thread.
    typedef int(__cdecl* Snprintf_t)(char* buffer, std::size_t size, const char* format, ...);
    Snprintf_t Snprintf = nullptr;
    std::optional<Patches::Patch<5>> zonePathCall;

    int __cdecl FormatZonePath(char* buffer, std::size_t size, const char* format, const char* basePath,
                               const char* name, const char* extension)
    {
        if (name && extension && (IsModZone(name) || std::string_view(name).starts_with(KIT_ZONE_PREFIX)))
        {
            auto path = (zonesFolder / (std::string(name) + extension)).string();
#ifdef IWXMVM_T5_HARNESS
            if (const auto extra = FindExtraZoneFile(name))
                path = extra->string();
#endif
            std::error_code error;
            if (path.size() < size && std::filesystem::is_regular_file(path, error))
            {
                std::memcpy(buffer, path.c_str(), path.size() + 1);
                return static_cast<int>(path.size());
            }
        }
        return Snprintf(buffer, size, format, basePath, name, extension);
    }

    void HookZonePath()
    {
        const auto callAddress = GetGameAddresses().DB_OpenZone_FormatCommonPath();
        if (*reinterpret_cast<const std::uint8_t*>(callAddress) != 0xE8)
        {
            LOG_ERROR("The zone loader's path isn't formatted with a call: character zones can't load");
            return;
        }
        const auto callOffset = *reinterpret_cast<const std::int32_t*>(callAddress + 1);
        Snprintf = reinterpret_cast<Snprintf_t>(callAddress + 5 + callOffset);
        std::array<std::uint8_t, 5> call{0xE8};
        const auto hookOffset =
            static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(FormatZonePath) - (callAddress + 5));
        std::memcpy(&call[1], &hookOffset, sizeof(hookOffset));
        // reverted when the mod is unloaded
        zonePathCall.emplace(callAddress, call, Patches::PatchApplySetting::Immediately);
    }

    // A sound zone's bank (a campaign level's or a Zombies map's, which has the sounds of its weapons' shots) is put
    // first in the sound system's list once added: the game looks an alias up from the last bank added to the first
    // (0x9002B0, 0x60EDF0, 0x5BBB70), so there it only adds the aliases no other bank has, and multiplayer's own sounds
    // of the same names stay theirs
    void PutBankLast(void* bank)
    {
        typedef void(__cdecl * Sys_CriticalSection_t)(std::int32_t section);
        const auto& addresses = GetGameAddresses();
        reinterpret_cast<Sys_CriticalSection_t>(addresses.Sys_EnterCriticalSection())(CRITSECT_SOUND);
        const auto banks = reinterpret_cast<void**>(addresses.snd_banks());
        const auto count = *reinterpret_cast<std::int32_t*>(addresses.snd_bankCount());
        if (const auto at = std::find(banks, banks + count, bank); at != banks + count)
            std::rotate(banks, at, at + 1);
        reinterpret_cast<Sys_CriticalSection_t>(addresses.Sys_LeaveCriticalSection())(CRITSECT_SOUND);
    }

    // whether the game has registered a weapon of a zone (a player of a live game was given it, or a film's weapon
    // configstrings name it), which it uses until the map goes: the zone stays loaded until then
    bool HasRegisteredWeapon(const CharacterZone& zone)
    {
        if (zone.weapons.empty() || zone.weaponRegistered)
            return zone.weaponRegistered;
        const auto variants = reinterpret_cast<void* const*>(GetGameAddresses().bg_weaponVariantDefs());
        for (std::size_t index = 1; index < Functions::GetWeaponVariantCount(); ++index)
            if (std::ranges::find(zone.weapons, variants[index]) != zone.weapons.end())
                return zone.weaponRegistered = true;
        return false;
    }

    void BumpGeneration();

    void UnloadPack();

    // A level's sound bank goes in the game's physical memory, up to 26 MB of the 352 MB the map's zones share, and one that
    // doesn't fit ends the game ("Out of memory error"). cuba leaves room for about five: a gun whose bank may not fit stays
    // silent. An inflated bank is at most 1.34 times its file.
    bool HasRoomForSoundBank(const std::filesystem::path& file)
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(file, error);
        return !error && size + size / 2 < PoolGrowth::GetPhysicalMemoryFree();
    }

    // the pools a character zone fills, with the room kept free in each before one loads (a character has up to about
    // 35 images; see LoadCharacterZone for one that doesn't fit)
    inline constexpr std::array CHARACTER_ZONE_POOLS{ASSET_TYPE_XMODEL, ASSET_TYPE_MATERIAL, ASSET_TYPE_TECHNIQUE_SET,
                                                     ASSET_TYPE_IMAGE};
    inline constexpr std::uint32_t POOL_ROOM_KEPT = 64;
    // The room a gun's zone leaves in those pools, for the gun has no way back once the game knows it. Materials are the ones
    // that run out, on cuba alone (83 free as a game starts): a gun takes 19 at most, 7 on
    // average, fewer after the first. It keeps the game off the pool's end, past which the driver's thread stopped the game.
    inline constexpr std::uint32_t WEAPON_ROOM_KEPT = 24;
    std::uint32_t weaponRoomKept = WEAPON_ROOM_KEPT;  // (the harness lowers it to reach the failure in LoadCharacterZone)

    void UnloadCharacterZones(std::int32_t bits);

    // Unloads one zone of the registered weapons' bit: the game uses the others until the map goes, so they do without it
    // meanwhile (the game unloads by flag bits, see CHARACTER_ZONE_BITS)
    void UnloadWeaponZone(const std::string& zone)
    {
        std::vector<std::int32_t*> others;
        for (std::size_t i = 1; i < PoolGrowth::GetZoneInfoCount(); ++i)
        {
            const auto info = PoolGrowth::GetZoneInfo() + i * ZONE_INFO_SIZE;
            const auto flags = reinterpret_cast<std::int32_t*>(info + ZONE_INFO_FLAGS);
            if ((*flags & REGISTERED_WEAPON_ZONE_BIT) && zone != reinterpret_cast<const char*>(info))
                others.push_back(flags);
        }
        for (const auto flags : others)
            *flags &= ~REGISTERED_WEAPON_ZONE_BIT;
        UnloadCharacterZones(REGISTERED_WEAPON_ZONE_BIT);
        for (const auto flags : others)
            *flags |= REGISTERED_WEAPON_ZONE_BIT;
    }

    LoadResult LoadCharacterZone(const std::string& zone, bool clientStarting, bool registeredWeapon)
    {
        LoadResult loadResult;
        ForgetUnloadedCharacterZones();
        if (FindCharacterZone(zone) || FindSoundZone(zone))
        {
            loadResult.loaded = true;
            return loadResult;
        }
        UnloadPack();
        // (a film's guns come before the first frame finishes a campaign level, and what the level left out waits in the
        // same lists as a zone's: released with a zone's, nothing spared what the level kept, and the film ended with
        // "Vertex type 2 doesn't have the information used by shader" on a wall)
        if (clientStarting)
            CampaignMaps::FinishLoad();
        // the game ends the demo when it can't find a zone
        if (!FindZoneFile(zone))
            loadResult.error = "there is no zone " + zone;
        else if (IsZoneLoaded(zone))
            loadResult.error = "a zone of that name is loaded already: " + zone;
        else if (GetLoadedZones().size() >= PoolGrowth::GetMaxZones())
            loadResult.error = std::format("the game has {} zones loaded, the most it can", PoolGrowth::GetMaxZones());
        else if (zone.starts_with(SOUND_ZONE_PREFIX) && !HasRoomForSoundBank(*FindZoneFile(zone)))
            loadResult.error = std::format("no room for its sounds ({} MB left of the game's zone memory)",
                                           PoolGrowth::GetPhysicalMemoryFree() >> 20);
        // (a gun has no way back once the game knows it, so it isn't tried; a film's always is, as it needs them all)
        else if (registeredWeapon && !clientStarting && !zone.starts_with(SOUND_ZONE_PREFIX) &&
                 std::ranges::any_of(CHARACTER_ZONE_POOLS, [](std::int32_t type) {
                     return CountFreeSlots(type) < weaponRoomKept;
                 }))
            loadResult.error = "the game has no room left for it";
        // (a weapon's zones also in a live game, as its player is given one, and as the client starts: see
        // WeaponZones; any zone in a game with live actors)
        else if ((Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo && !LiveActors::IsLive() &&
                  !((Mod::GetGameInterface()->GetGameState() == Types::GameState::InGame || clientStarting) &&
                    (zone.starts_with(WEAPON_ZONE_PREFIX) || zone.starts_with(SOUND_ZONE_PREFIX)))) ||
                 (!GetClientGlobals() && !clientStarting) || unloaded)
            loadResult.error = "zones load in a demo";
        if (!loadResult.error.empty())
            return loadResult;

        std::unique_lock assets(assetMutex);
        loading = true;
        result = &loadResult;
        loadResult.memoryBefore = GetPrivateBytes();
        const auto started = std::chrono::steady_clock::now();

        loadingCharacterZone = std::make_unique<CharacterZone>();
        auto& loaded = *loadingCharacterZone;
        loaded.name = zone;
        // (a sound zone in the game's zone memory, see soundZones)
        const bool sound = zone.starts_with(SOUND_ZONE_PREFIX);
        loaded.flags = sound              ? PACK_ZONE_FLAGS
                       : registeredWeapon ? CHARACTER_ZONE_MEMORY | REGISTERED_WEAPON_ZONE_BIT
                                          : CHARACTER_ZONE_MEMORY | ChooseCharacterZoneBit();
        // (a weapon's zone loaded for a player stays from now, not only once the game has registered the weapon: in
        // between, a live bot's gun was unloaded as no actor needed it while the address space was short, and the
        // bot was given nothing, live-actors in the full suite of 2026-11-08)
        loaded.weaponRegistered = registeredWeapon && !sound;
        packModels.clear();
        duplicates.clear();
        const auto copiesBefore = headerCopies.size();
        const auto constantTablesBefore = constantTables.size();
        const auto textureTablesBefore = textureTables.size();

        packZones = {{zone, Keep::All}};
        pass = Pass::Load;
        std::vector<XZoneInfo> zones{{zone.c_str(), loaded.flags, sound ? FREE_EXACTLY : 0}};
        LoadXAssets(zones, !clientStarting);
        pass = Pass::None;
        packZones.clear();
        RemoveSoundCopies();
        ReleaseCopies();
        ReleaseDuplicates(loaded);
        MakeDrawable(loadResult);
        CreateModelBuffers();

        // what was made for the zone goes with it
        for (auto i = copiesBefore; i < headerCopies.size(); ++i)
        {
            copies.erase(headerCopies[i].get());
            loaded.headerCopies.push_back(std::move(headerCopies[i]));
        }
        headerCopies.resize(copiesBefore);
        std::move(constantTables.begin() + constantTablesBefore, constantTables.end(),
                  std::back_inserter(loaded.constantTables));
        constantTables.resize(constantTablesBefore);
        std::move(textureTables.begin() + textureTablesBefore, textureTables.end(),
                  std::back_inserter(loaded.textureTables));
        textureTables.resize(textureTablesBefore);
        loaded.models = loadResult.models;

        loadResult.loaded = IsZoneLoaded(zone);
        if (!loadResult.loaded)
            loadResult.error = "the game didn't load " + zone;
        else
        {
            if (loaded.soundBank)
                PutBankLast(loaded.soundBank);
            std::lock_guard lock(characterZonesMutex);
            (sound ? soundZones : characterZones).push_back(std::move(loaded));
        }
        loadingCharacterZone.reset();
        loadResult.memoryAfter = GetPrivateBytes();
        loadResult.milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        result = nullptr;
        loading = false;
        assets.unlock();

        // What had no room in its pool is a copy no other asset of the game knows, which can't be drawn (the renderer
        // finds images by their place in the pool) and whose resources are released (ReleaseCopies): the zone goes
        // again before anything draws it. RunCharacterZoneRequests makes room before a load, but a map may leave
        // little.
        if (loadResult.loaded && std::ranges::any_of(loadResult.poolFull, [](std::uint32_t n) { return n != 0; }))
        {
            if (registeredWeapon)
                UnloadWeaponZone(zone);
            else
                UnloadCharacterZone(zone);
            loadResult.loaded = false;
            loadResult.error = "the game has no room left for all of " + zone;
        }
        BumpGeneration();
        return loadResult;
    }

    // Readies the zones about to be unloaded (the actors made of their models released). After zones are unloaded the
    // game marks what the assets of the zones still loaded use as in use (each entry's inuse) and frees the stand-ins
    // of the rest, but it never unmarks anything, so whatever was in use once (a character zone loaded while another
    // was unloaded) would stay as a stand-in for good, taking its place in its pool: the entries of the zones are
    // unmarked first, and the game marks again what still uses them (every asset of a loaded zone ends up marked).
    // Their models' buffers are released on the render thread (see ReleaseModelBuffersOnRenderThread).
    template <typename IsUnloading, typename Function>
    void ForEachEntryOf(IsUnloading isUnloading, Function function)  // a zone's flags -> whether it is about to go
    {
        // (an entry holds its zone's index in a byte)
        std::array<bool, 0x100> unloading{};
        for (std::size_t i = 1; i < PoolGrowth::GetZoneInfoCount(); ++i)
        {
            const auto info = PoolGrowth::GetZoneInfo() + i * ZONE_INFO_SIZE;
            const auto flags = *reinterpret_cast<const std::int32_t*>(info + ZONE_INFO_FLAGS);
            unloading[i] = *reinterpret_cast<const char*>(info) && isUnloading(flags);
        }
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
                if (unloading[entries[index].zoneIndex])
                    function(entries[index]);
    }

    template <typename IsUnloading>
    void ReleaseModelBuffersOnRenderThreadOf(IsUnloading isUnloading)
    {
        ForEachEntryOf(isUnloading, [](XAssetEntry& entry) {
            if (entry.type == ASSET_TYPE_XMODEL && entry.header)
                ReleaseModelBuffersOnRenderThread(*static_cast<XModel*>(const_cast<void*>(entry.header)));
        });
    }

    template <typename IsUnloading>
    void PrepareUnloadOf(IsUnloading isUnloading)
    {
        ForEachEntryOf(isUnloading, [](XAssetEntry& entry) { entry.inuse = 0; });
        ReleaseModelBuffersOnRenderThreadOf(isUnloading);
    }

    // of the character zones of the flag bits
    void PrepareUnload(std::int32_t bits)
    {
        PrepareUnloadOf([bits](std::int32_t flags) { return (flags & CHARACTER_ZONE_MEMORY) && (flags & bits); });
    }

    // unloads the character zones of the flag bits (the actors made of their models released first)
    void UnloadCharacterZones(std::int32_t bits)
    {
        if (!(bits & ALL_CHARACTER_ZONE_BITS))
            return;
        ReleaseActorsForUnload();
        {
            std::unique_lock assets(assetMutex);
            loading = true;
            PrepareUnload(bits & ALL_CHARACTER_ZONE_BITS);
            std::vector<XZoneInfo> zones{{nullptr, 0, FREE_EXACTLY | (bits & ALL_CHARACTER_ZONE_BITS)}};
            LoadXAssets(zones);
            loading = false;
        }
        ForgetUnloadedCharacterZones();
        BumpGeneration();
    }

    // What unloading a character zone takes with it: the zones that use its assets and those of its flag bit, and so
    // on, and their flag bits
    std::pair<std::set<std::string>, std::int32_t> GetUnloadSet(const std::string& zone)
    {
        std::set<std::string> going{zone};
        std::int32_t bits = 0;
        for (std::size_t count = 0; count != going.size();)
        {
            count = going.size();
            for (const auto& other : characterZones)
                if (going.contains(other.name))
                    bits |= other.flags & ALL_CHARACTER_ZONE_BITS;
            for (const auto& other : characterZones)
                if ((other.flags & bits) ||
                    std::ranges::any_of(other.dependsOn, [&](const auto& name) { return going.contains(name); }))
                    going.insert(other.name);
        }
        return {going, bits};
    }

    std::vector<std::string> UnloadCharacterZone(const std::string& zone)
    {
        ForgetUnloadedCharacterZones();
        if (!FindCharacterZone(zone))
            return {};
        const auto [going, bits] = GetUnloadSet(zone);
        UnloadCharacterZones(bits);
        return {going.begin(), going.end()};
    }

    void UnloadAllCharacterZones()
    {
        ForgetUnloadedCharacterZones();
        std::int32_t bits = 0;
        for (const auto& zone : characterZones)
            bits |= zone.flags;
        UnloadCharacterZones(bits);
    }

    // of zones that didn't load, until the next demo (on the main thread), and the latest of them (any thread)
    std::unordered_map<std::string, std::string> zoneErrors;
    std::mutex zoneErrorMutex;
    std::string lastZoneError;

    Components::AddressSpaceLogic::AddressSpace MeasureAddressSpace();

    void UnloadCharacterZonesForDemo()
    {
        // (a line for each film in the log: the space a long game has left)
        LOG_INFO("The game's address space as a demo loads: {}",
                 Components::AddressSpaceLogic::Describe(MeasureAddressSpace()));
        UnloadAllCharacterZones();
        // a zone that didn't load may load in the next demo (the setup may have run meanwhile)
        zoneErrors.clear();
        std::lock_guard lock(zoneErrorMutex);
        lastZoneError.clear();
    }

    // How many character zones may be loaded, and how much their textures may take, before those no actor needs are
    // unloaded (the least recently needed first); the harness lowers them to test it
    std::size_t zoneBudget = 16;
    std::uint64_t textureBudget = 512ull << 20;
    // the zones the game can have loaded at all; one is kept free for a pack
    inline constexpr std::size_t ZONES_KEPT_FREE = 1;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastNeeded;
    // (the harness's: see HoldCharacterZoneLoads)
    std::atomic<bool> zoneLoadsHeld = false;

    // The game's 32-bit address space, read once a second while character zones are asked for: short of it, the zones
    // no actor needs go whatever the budget. Each holds about 10 MB of the process's
    // heap, and 16 of them were kept for nothing in a long game. The harness sets another floor (0 for the default).
    std::atomic<std::uint64_t> addressSpaceFloor = 0;
    bool addressSpaceShort = false;
    std::chrono::steady_clock::time_point addressSpaceRead;
    std::atomic<bool> addressSpaceAsked = false;  // read again at the next frame (the floor changed)

    Components::AddressSpaceLogic::AddressSpace MeasureAddressSpace()
    {
        Components::AddressSpaceLogic::AddressSpace space;
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t address = 0x10000; VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info));)
        {
            if (info.State == MEM_FREE)
            {
                space.free += info.RegionSize;
                space.largest = std::max<std::uint64_t>(space.largest, info.RegionSize);
            }
            const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (next <= address)
                break;
            address = next;
        }
        return space;
    }

    void ReadAddressSpace()
    {
        const auto now = std::chrono::steady_clock::now();
        if (!addressSpaceAsked.exchange(false) && now - addressSpaceRead < std::chrono::seconds(1))
            return;
        addressSpaceRead = now;
        const auto space = MeasureAddressSpace();
        const auto floor = addressSpaceFloor.load();
        const bool wasShort = std::exchange(
            addressSpaceShort, Components::AddressSpaceLogic::IsShort(
                                   space, floor ? floor : Components::AddressSpaceLogic::FREE_FLOOR));
        if (addressSpaceShort && !wasShort)
            LOG_WARN("The game's address space is short ({}): the character zones no actor needs go",
                     Components::AddressSpaceLogic::Describe(space));
    }

    // whether the game is capturing frames, when nothing may stall it
    bool IsCapturing();

    // Unloads every sound zone (see soundZones), the game's way, as a pack goes: in the game's zone memory they can only
    // go together, the last loaded first. Main thread.
    void UnloadSoundZones()
    {
        if (soundZones.empty())
            return;
        {
            std::unique_lock assets(assetMutex);
            loading = true;
            PrepareUnloadOf(
                [](std::int32_t flags) { return !(flags & CHARACTER_ZONE_MEMORY) && (flags & PACK_ZONE_FLAGS); });
            std::vector<XZoneInfo> zones{{nullptr, 0, PACK_ZONE_FLAGS}};
            LoadXAssets(zones);
            loading = false;
        }
        ForgetUnloadedCharacterZones();
        BumpGeneration();
    }

    void UnloadUnneededCharacterZones(const std::vector<std::string>& wanted)
    {
        // (players and corpses may be built of a zone's models still, whoever chose them: the game gives them others
        // only as it next draws them)
        std::set<std::string, std::less<>> playersModels;
        for (std::int32_t number = 0; number < static_cast<std::int32_t>(FIRST_CORPSE_ENTITY + MAX_CORPSES); ++number)
            if (const auto obj = Functions::Com_GetClientDObj(number, 0))
                for (int i = 0; i < obj->numModels; ++i)
                    playersModels.insert(obj->models[i]->name);
        const auto isOnPlayers = [&](const CharacterZone& zone) {
            return std::ranges::any_of(zone.models,
                                       [&](const std::string& model) { return playersModels.contains(model); });
        };
        // (the room the game's other zones leave, the map's, the sound zones' and those that stay for their registered
        // weapons, as they are now; those don't count in the budget, which is for what can go)
        const auto staying = static_cast<std::size_t>(std::ranges::count_if(characterZones, HasRegisteredWeapon));
        const auto maxZones = [&] {
            const auto otherZones = GetLoadedZones().size() - characterZones.size() + staying;
            const auto room = PoolGrowth::GetMaxZones() - std::min(otherZones + ZONES_KEPT_FREE, PoolGrowth::GetMaxZones());
            return std::min(zoneBudget, room);
        };
        const auto overBudget = [&] {
            std::uint64_t textures = 0;
            for (const auto& zone : characterZones)
                for (const auto image : zone.images)
                    textures += image->cardMemory;
            return characterZones.size() - staying > maxZones() || textures > textureBudget || addressSpaceShort ||
                   std::ranges::any_of(CHARACTER_ZONE_POOLS,
                                       [](std::int32_t type) { return CountFreeSlots(type) < POOL_ROOM_KEPT; });
        };
        // The sound zones first, all together, when the game's zones are what's short and one of them no actor's weapon
        // needs: they take the room of character zones (see PoolGrowth::GetMaxZones), and those still needed load again.
        // For anything else they'd free nothing, and a live game would lose its shots' sounds.
        if (overBudget() && maxZones() < zoneBudget && std::ranges::any_of(soundZones, [&](const CharacterZone& zone) {
                return std::ranges::find(wanted, zone.name) == wanted.end();
            }))
            UnloadSoundZones();
        while (overBudget())
        {
            // the zone needed longest ago whose unload takes no zone an actor needs
            const CharacterZone* victim = nullptr;
            for (const auto& zone : characterZones)
            {
                if (std::ranges::find(wanted, zone.name) != wanted.end())
                    continue;
                // (nor one whose weapon the game registered, nor one a player or corpse is made of)
                const auto [going, bits] = GetUnloadSet(zone.name);
                if (std::ranges::any_of(going, [&](const std::string& name) {
                        const auto other = FindCharacterZone(name);
                        return std::ranges::find(wanted, name) != wanted.end() ||
                               (other && (HasRegisteredWeapon(*other) || isOnPlayers(*other)));
                    }))
                    continue;
                if (!victim || lastNeeded[zone.name] < lastNeeded[victim->name])
                    victim = &zone;
            }
            if (!victim)
                return;
            const auto name = victim->name;
            const auto gone = UnloadCharacterZone(name);
            for (const auto& zone : gone)
                lastNeeded.erase(zone);
        }
    }

    // Multiplayer draws its heads with this technique set, the skin's with a sampler of the face paint: an empty image
    // (c_gen_facepaint_0) the game swaps for the paint as it draws a player (CG_GetWeaponMaterialOverride, see
    // RENDER_OPTIONS_FACE_PAINT_SHIFT). Campaign heads are made the same way, with the same layout, but drawn without it:
    // the heads of a character zone that actors or players have paint on get it, and the
    // empty image, from a head of the map, as a material gets what it lacks in MakeDrawable. It stays until the zone
    // goes, looking the same without paint. Heads nobody paints stay as the game draws them (some with the skin
    // tension of their faces). A map without multiplayer's heads (a campaign level) has no such technique set.
    inline constexpr auto FACE_PAINT_TECHNIQUE_SET = "mc_sw4_3d_char_skin_facepaint";
    inline constexpr std::string_view SKIN_TECHNIQUE_SET = "mc_sw4_3d_char_skin";

    bool IsPaintableHead(const Material& material)
    {
        return std::string_view(material.techniqueSet->name).starts_with(SKIN_TECHNIQUE_SET) &&
               std::string_view(material.name).find("head") != std::string_view::npos;
    }

    bool CanHaveFacePaint(const XModel& model)
    {
        if (!FindRegistered(ASSET_TYPE_TECHNIQUE_SET, FACE_PAINT_TECHNIQUE_SET) || !model.materialHandles)
            return false;
        return std::ranges::any_of(std::span(model.materialHandles, model.numsurfs), [](const Material* material) {
            return material && IsPaintableHead(*material);
        });
    }

    void AddFacePaint(const std::vector<std::string>& models)
    {
        const auto paint =
            static_cast<MaterialTechniqueSet*>(FindRegistered(ASSET_TYPE_TECHNIQUE_SET, FACE_PAINT_TECHNIQUE_SET));
        if (!paint)
            return;

        // by the zones they are in, which the tables made for them go with
        std::vector<std::pair<CharacterZone*, Material*>> heads;
        for (auto& zone : characterZones)
        {
            for (const auto& name : zone.models)
            {
                if (std::ranges::find(models, name) == models.end())
                    continue;
                const auto model = static_cast<XModel*>(FindRegistered(ASSET_TYPE_XMODEL, name.c_str()));
                for (std::uint32_t surface = 0; model && model->materialHandles && surface < model->numsurfs; ++surface)
                {
                    const auto material = model->materialHandles[surface];
                    if (material && material->techniqueSet != paint && IsPaintableHead(*material))
                        heads.emplace_back(&zone, material);
                }
            }
        }
        if (heads.empty())
            return;

        const auto arguments = GetMaterialArguments(*paint);
        const auto from = FindMaterialWith(paint, arguments);
        if (!from)
            return;
        // (the render thread draws with these materials)
        SyncRenderThread();
        for (const auto& [zone, material] : heads)
        {
            AddArguments(*material, GetMissingArguments(*material, arguments), *from);
            material->techniqueSet = paint;
            std::ranges::move(constantTables, std::back_inserter(zone->constantTables));
            std::ranges::move(textureTables, std::back_inserter(zone->textureTables));
            constantTables.clear();
            textureTables.clear();
        }
    }

    void RunCharacterZoneRequests()
    {
        if (unloaded || loading ||
            (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo && !LiveActors::IsLive()) ||
            !GetClientGlobals() || IsCapturing())
            return;
        AddFacePaint(Components::Actors::GetFacePaintModels());
        auto wanted = Components::Actors::GetWantedCharacterZones();
        // and the sound zones of the weapons the game registered (a film's), whose shots play from them
        for (const auto& zone : characterZones)
            if (HasRegisteredWeapon(zone))
                for (const auto& weapon : Components::WeaponCatalog::Get().weapons)
                    for (const auto& variant : weapon.variants)
                        if (variant.zone == zone.name && !variant.sounds.empty() &&
                            std::ranges::find(wanted, variant.sounds) == wanted.end())
                            wanted.push_back(variant.sounds);
        const auto now = std::chrono::steady_clock::now();
        for (const auto& zone : wanted)
            lastNeeded[zone] = now;
        ForgetUnloadedCharacterZones();
        ReadAddressSpace();
        if (zoneLoadsHeld)
            return;

        // one a frame, so the UI shows what is loading
        const auto missing = std::ranges::find_if(wanted, [](const std::string& zone) {
            return !FindCharacterZone(zone) && !FindSoundZone(zone) && !zoneErrors.contains(zone);
        });
        if (missing != wanted.end())
        {
            // first room for it, then the campaign's animations, which their actors play (once a demo, like packs),
            // unless the store loads them from the animation pack (see AnimationStore): the zones need nothing else of
            // single player common
            UnloadUnneededCharacterZones(wanted);
            if (!animationsLoaded && !AnimationStore::IsReady())
            {
                std::unique_lock assets(assetMutex);
                loading = true;
                LoadResult loadResult;
                result = &loadResult;
                LoadAnimations();
                result = nullptr;
                loading = false;
                assets.unlock();
                BumpGeneration();
                return;
            }
            const auto loaded = LoadCharacterZone(*missing);
            if (!loaded.error.empty())
            {
                zoneErrors[*missing] = loaded.error;
                {
                    std::lock_guard lock(zoneErrorMutex);
                    lastZoneError = std::format("{}: {}", *missing, loaded.error);
                }
                LOG_ERROR("The character zone {} didn't load: {}", *missing, loaded.error);
                BumpGeneration();
            }
            return;
        }
        UnloadUnneededCharacterZones(wanted);
    }

    void HoldCharacterZoneLoads(bool hold)
    {
        zoneLoadsHeld = hold;
    }

    void SetCharacterZoneBudget(std::size_t zones, std::uint64_t textureBytes)
    {
        zoneBudget = zones;
        textureBudget = textureBytes;
    }

    void SetAddressSpaceFloor(std::uint64_t bytes)
    {
        addressSpaceFloor = bytes;
        addressSpaceAsked = true;
    }

    void SetWeaponRoomKept(std::uint32_t room)
    {
        weaponRoomKept = room;
    }

    std::string GetCharacterZoneError()
    {
        std::lock_guard lock(zoneErrorMutex);
        return lastZoneError;
    }

    std::vector<CharacterZoneInfo> GetCharacterZones()
    {
        std::vector<CharacterZoneInfo> zones;
        std::lock_guard lock(characterZonesMutex);
        // (and the sound zones, see soundZones)
        for (const auto& zone : soundZones)
            zones.push_back({.name = zone.name, .flags = zone.flags});
        for (const auto& zone : characterZones)
        {
            auto& info =
                zones.emplace_back(CharacterZoneInfo{.name = zone.name,
                                                     .flags = zone.flags,
                                                     .models = zone.models,
                                                     .dependsOn = {zone.dependsOn.begin(), zone.dependsOn.end()},
                                                     .usesPack = zone.usesPack});
            for (const auto image : zone.images)
                info.textureBytes += image->cardMemory;
        }
        return zones;
    }

    bool HasGameZone(const std::string& zone)
    {
        return FindZoneFile(zone).has_value();
    }

    bool HasZoneFailed(const std::string& zone)
    {
        return zoneErrors.contains(zone);
    }

    std::vector<void*> GetZoneWeapons(const std::string& zone)
    {
        std::vector<void*> weapons;
        if (const auto loaded = FindCharacterZone(zone))
            for (const auto weapon : loaded->weapons)
                weapons.push_back(const_cast<void*>(weapon));
        return weapons;
    }

    bool IsCharacterZoneLoaded(const std::string& zone)
    {
        std::lock_guard lock(characterZonesMutex);
        return std::ranges::any_of(characterZones, [&](const CharacterZone& loaded) { return loaded.name == zone; }) ||
               std::ranges::any_of(soundZones, [&](const CharacterZone& loaded) { return loaded.name == zone; });
    }

    void* FindAsset(std::int32_t type, const char* name)
    {
        return type >= 0 && type < ASSET_TYPE_COUNT ? FindRegistered(type, name) : nullptr;
    }

    const std::vector<PackInfo>& GetAvailablePacks()
    {
        static std::vector<PackInfo> packs = [] {
            // in campaign order
            static constexpr std::array<std::array<const char*, 3>, 29> KNOWN{{
                {"frontend", "Interrogation", "Campaign"},
                {"cuba", "Operation 40", "Campaign"},
                {"vorkuta", "Vorkuta", "Campaign"},
                {"pentagon", "U.S.D.D.", "Campaign"},
                {"flashpoint", "Executive Order", "Campaign"},
                {"khe_sanh", "S.O.G.", "Campaign"},
                {"hue_city", "The Defector", "Campaign"},
                {"kowloon", "Numbers", "Campaign"},
                {"fullahead", "Project Nova", "Campaign"},
                {"creek_1", "Victor Charlie", "Campaign"},
                {"river", "Crash Site", "Campaign"},
                {"wmd_sr71", "WMD (SR-71)", "Campaign"},
                {"wmd", "WMD", "Campaign"},
                {"pow", "Payback", "Campaign"},
                {"rebirth", "Rebirth", "Campaign"},
                {"int_escape", "Revelations", "Campaign"},
                {"underwaterbase", "Redemption", "Campaign"},
                {"outro", "Epilogue", "Campaign"},
                {"zombie_theater", "Kino der Toten", "Zombies"},
                {"zombie_pentagon", "Five", "Zombies"},
                {"zombie_cosmodrome", "Ascension", "Zombies"},
                {"zombie_coast", "Call of the Dead", "Zombies"},
                {"zombie_temple", "Shangri-La", "Zombies"},
                {"zombie_moon", "Moon", "Zombies"},
                {"zombie_cod5_prototype", "Nacht der Untoten", "Zombies"},
                {"zombie_cod5_asylum", "Verruckt", "Zombies"},
                {"zombie_cod5_sumpf", "Shi No Numa", "Zombies"},
                {"zombie_cod5_factory", "Der Riese", "Zombies"},
                {"zombietron", "Dead Ops Arcade", "Zombies"},
            }};
            std::vector<PackInfo> available;
            const auto basePath = Functions::FindDvar("fs_basepath");
            const std::filesystem::path zones =
                basePath && basePath->current.string
                    ? std::filesystem::path(basePath->current.string) / "zone" / "Common"
                    : std::filesystem::path();
            for (const auto& [name, displayName, group] : KNOWN)
            {
                std::error_code error;
                if (zones.empty() || std::filesystem::exists(zones / (std::string(name) + ".ff"), error))
                    available.push_back({name, displayName, group});
            }
            return available;
        }();
        return packs;
    }

    std::string requestedPack;
    bool packUnloadRequested = false;  // no pack chosen
    LoadResult lastLoadResult;

    void RequestLoad(const std::string& zone)
    {
        std::lock_guard lock(packMutex);
        if (zone == packState.loaded && packState.error.empty())
            return;
        requestedPack = zone;
        packUnloadRequested = zone.empty();
        packState.loading = zone;
        ++packState.generation;
    }

    // A pack and character zones are never loaded together: each first unloads the other (character zones of a level
    // share what its pack registered, take the stand-ins of what an earlier pack left in use and fill them, and
    // unloading them then took images the pack's materials drew with; the shader warm-up after the next change of
    // the materials drew with a freed image and hung in the driver, as after actors-packs and view-resize's unload).
    // The character zones go first, as they may use the pack's assets. On the main thread.
    void UnloadPack()
    {
        if (packZone.empty())
            return;
        if (IsZoneLoaded(packZone))
        {
            UnloadAllCharacterZones();
            ReleaseActorsForUnload();
            std::unique_lock assets(assetMutex);
            loading = true;
            PrepareUnloadOf(
                [](std::int32_t flags) { return !(flags & CHARACTER_ZONE_MEMORY) && (flags & PACK_ZONE_FLAGS); });
            std::vector<XZoneInfo> zones{{nullptr, 0, PACK_ZONE_FLAGS}};
            LoadXAssets(zones);
            loading = false;
        }
        // (the tables made for its materials went with them)
        constantTables.clear();
        textureTables.clear();
        packTechniqueSets.clear();
        packZone.clear();
        std::lock_guard lock(packMutex);
        packState.loaded.clear();
        packState.models.clear();
        ++packState.generation;
    }

    void RunRequestedLoad()
    {
        std::string zone;
        {
            std::lock_guard lock(packMutex);
            if ((requestedPack.empty() && !packUnloadRequested) ||
                Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo || !GetClientGlobals() || unloaded)
                return;
            zone = std::exchange(requestedPack, {});
            packUnloadRequested = false;
        }
        if (zone.empty())
        {
            UnloadPack();
            std::lock_guard lock(packMutex);
            packState.loading.clear();
            packState.error.clear();
            ++packState.generation;
            return;
        }
        if (std::ranges::find(GetAvailablePacks(), zone, &PackInfo::name) == GetAvailablePacks().end())
        {
            std::lock_guard lock(packMutex);
            packState.loading.clear();
            packState.error = "There is no pack " + zone;
            ++packState.generation;
            return;
        }

        // the models of the pack before go away with its zone, and the character zones, which may use its assets (or
        // the pack theirs)
        ReleaseActorsForUnload();
        UnloadAllCharacterZones();
        std::unique_lock assets(assetMutex);
        loading = true;
        // the pack before goes as this one loads (as its survey does)
        if (zone != ANIMATIONS_ZONE)
            ReleaseModelBuffersOnRenderThreadOf(
                [](std::int32_t flags) { return !(flags & CHARACTER_ZONE_MEMORY) && (flags & PACK_ZONE_FLAGS); });
        const auto loaded = Load(zone);
        loading = false;
        assets.unlock();

        std::lock_guard lock(packMutex);
        lastLoadResult = loaded;
        if (packState.loading == zone)
            packState.loading.clear();
        packState.loaded = loaded.loaded ? zone : std::string();
        packState.error = loaded.error;
        packState.memoryMegabytes = static_cast<std::int32_t>(loaded.memoryAfter >> 20);
        packState.models = loaded.models;
        ++packState.generation;
    }

    void Unload()
    {
        MapMenu::Unload();
        CampaignMaps::Unload();
        std::lock_guard lock(packMutex);
        unloaded = true;
        requestedPack.clear();
        packUnloadRequested = false;
        // left to the process: what the mod frees as it unloads would be used by the game after it
        for (auto& copy : headerCopies)
            static_cast<void>(copy.release());
        for (auto& table : constantTables)
            static_cast<void>(table.release());
        for (auto& table : textureTables)
            static_cast<void>(table.release());
        headerCopies.clear();
        constantTables.clear();
        textureTables.clear();
        // and so are the character zones' (STOP unloads them, see UnloadZonesWithClient)
        std::lock_guard zonesLock(characterZonesMutex);
        for (auto& zone : characterZones)
        {
            for (auto& copy : zone.headerCopies)
                static_cast<void>(copy.release());
            for (auto& table : zone.constantTables)
                static_cast<void>(table.release());
            for (auto& table : zone.textureTables)
                static_cast<void>(table.release());
        }
        characterZones.clear();
    }

    bool IsLoading()
    {
        return loading;
    }

    std::shared_lock<std::shared_mutex> TryReadAssets()
    {
        return std::shared_lock(assetMutex, std::try_to_lock);
    }

    PackState GetPackState()
    {
        std::lock_guard lock(packMutex);
        return packState;
    }

    void BumpGeneration()
    {
        std::lock_guard lock(packMutex);
        ++packState.generation;
    }

    LoadResult GetLastLoadResult()
    {
        std::lock_guard lock(packMutex);
        return lastLoadResult;
    }

    std::vector<std::string> FindIncompleteMaterials()
    {
        std::vector<std::string> incomplete;
        std::unordered_map<const MaterialTechniqueSet*, MaterialArguments> arguments;
        const auto hashTable = reinterpret_cast<const std::uint16_t*>(GetGameAddresses().db_hashTable());
        const auto entries = reinterpret_cast<const XAssetEntry*>(GetGameAddresses().g_assetEntryPool());
        for (std::size_t hash = 0; hash < DB_HASH_TABLE_SIZE; ++hash)
        {
            for (auto index = hashTable[hash]; index != 0; index = entries[index].nextHash)
            {
                const auto& entry = entries[index];
                const auto material = static_cast<const Material*>(entry.header);
                if (entry.type != ASSET_TYPE_MATERIAL || !material || !material->techniqueSet)
                    continue;
                if (!arguments.contains(material->techniqueSet))
                    arguments[material->techniqueSet] = GetMaterialArguments(*material->techniqueSet);
                const auto missing = GetMissingArguments(*material, arguments[material->techniqueSet]);
                const auto sorted =
                    std::ranges::is_sorted(std::span(material->constantTable, material->constantCount), {},
                                           &MaterialConstantDef::nameHash) &&
                    std::ranges::is_sorted(std::span(material->textureTable, material->textureCount), {},
                                           &MaterialTextureDef::nameHash);
                if (missing.constants.empty() && missing.samplers.empty() && sorted)
                    continue;
                std::string hashes;
                for (const auto nameHash : missing.constants)
                    hashes += std::format(" c{:08x}", nameHash);
                for (const auto nameHash : missing.samplers)
                    hashes += std::format(" s{:08x}", nameHash);
                incomplete.push_back(std::format(
                    "{} ({}{}): {} constants, {} textures{}, lacks{}", material->name, material->techniqueSet->name,
                    IsOwnTechniqueSet(material->techniqueSet) ? ", pack" : "", material->constantCount,
                    material->textureCount, sorted ? "" : " out of order", hashes));
            }
        }
        return incomplete;
    }

    // Leaving a demo for the menu (STOP next to it) frees the client's own memory, "PerLocalClient", right after
    // unloading the menus' zones. The game's physical memory is a stack, where only what was allocated last can be
    // freed, and common's and the pack's zones come after the client's memory: with common loaded that free ended the
    // game ("free does not match allocation", in an error box saying "Disconnected from server"). The game had unloaded
    // the pack's zone by then, but never common's. So both are unloaded with the menus' zones (the pack's first, as it
    // came last; no zone of the game has either flag). Loading another demo keeps the client's memory and reloads them.
    // The character zones, which aren't in that memory, go too (before the pack, which they may use): they use assets
    // of the map's zones, which go with the client. The change stays when the mod is unloaded, as its zones do.
    void UnloadZonesWithClient()
    {
        constexpr std::int32_t MENU_ZONE_FLAGS = 0x6000000;
        constexpr std::int32_t ZONE_FLAGS = PACK_ZONE_FLAGS | ANIMATIONS_ZONE_FLAGS | ALL_CHARACTER_ZONE_BITS;
        const auto address = GetGameAddresses().UnloadMenuZones_FreeFlags();
        const auto flags = *reinterpret_cast<const std::int32_t*>(address);
        if ((flags & ~(ZONE_FLAGS | MOD_ZONE_FLAGS)) != MENU_ZONE_FLAGS)
        {
            LOG_ERROR(
                "The client unloads zones of flags {:X} as it leaves a demo, not the menus' alone: leaving a "
                "demo after loading a character pack may end the game",
                flags);
            return;
        }
        if ((flags & ~MENU_ZONE_FLAGS) == ZONE_FLAGS)
            return;

        std::array<std::uint8_t, 4> bytes{};
        const auto wanted = MENU_ZONE_FLAGS | ZONE_FLAGS;
        std::memcpy(bytes.data(), &wanted, bytes.size());
        // (never destroyed, so never reverted)
        static_cast<void>(new Patches::Patch<4>(address, bytes, Patches::PatchApplySetting::Immediately));
    }

    // The assets of the zones that go with the client are all marked in use (see PrepareUnloadOf), so the game would
    // keep every one of the mod's as a stand-in, each taking its pool's slot for good: the next demo then had no room
    // to load common's animations again (934 of its 1333 stayed stand-ins), nor in the end a character zone. So the
    // call of DB_LoadXAssets that unloads them comes here, which releases the actors and readies the mod's zones
    // (PrepareUnloadOf) first. On the main thread.
    typedef void(__cdecl* DB_LoadXAssets_t)(XZoneInfo* zones, std::uint32_t count, std::int32_t sync);
    DB_LoadXAssets_t ClientZonesLoadXAssets = nullptr;
    std::optional<Patches::Patch<5>> clientZonesCall;

    void __cdecl UnloadZonesWithClient_Hook(XZoneInfo* zones, std::uint32_t count, std::int32_t sync)
    {
        std::int32_t freeing = 0;
        for (std::uint32_t i = 0; i < count; ++i)
            freeing |= zones[i].freeFlags;
        if (!(freeing & (PACK_ZONE_FLAGS | ANIMATIONS_ZONE_FLAGS | ALL_CHARACTER_ZONE_BITS)))
        {
            ClientZonesLoadXAssets(zones, count, sync);
            return;
        }

        ReleaseActorsForUnload();
        {
            std::unique_lock assets(assetMutex);
            loading = true;
            PrepareUnloadOf([freeing](std::int32_t flags) {
                if (flags & CHARACTER_ZONE_MEMORY)
                    return (flags & freeing & ALL_CHARACTER_ZONE_BITS) != 0;
                return (flags & freeing & (PACK_ZONE_FLAGS | ANIMATIONS_ZONE_FLAGS)) != 0;
            });
            ClientZonesLoadXAssets(zones, count, sync);
            loading = false;
        }
        ForgetUnloadedCharacterZones();
        BumpGeneration();
    }

    void HookUnloadZonesWithClient()
    {
        // the call follows its free flags (push 1, push ecx, ..., mov [esp+18h], flags, call DB_LoadXAssets)
        const auto callAddress = GetGameAddresses().UnloadMenuZones_FreeFlags() + 4;
        const auto target = callAddress + 5 + *reinterpret_cast<const std::int32_t*>(callAddress + 1);
        if (*reinterpret_cast<const std::uint8_t*>(callAddress) != 0xE8 ||
            target != GetGameAddresses().DB_LoadXAssets())
        {
            LOG_ERROR("The client's unload of zones isn't the call expected: leaving a demo leaves the mod's assets in "
                      "their pools");
            return;
        }
        ClientZonesLoadXAssets = reinterpret_cast<DB_LoadXAssets_t>(target);
        std::array<std::uint8_t, 5> call{0xE8};
        const auto hookOffset = static_cast<std::int32_t>(
            reinterpret_cast<std::uintptr_t>(UnloadZonesWithClient_Hook) - (callAddress + 5));
        std::memcpy(&call[1], &hookOffset, sizeof(hookOffset));
        // reverted when the mod is unloaded
        clientZonesCall.emplace(callAddress, call, Patches::PatchApplySetting::Immediately);
    }

    bool IsCapturing()
    {
        return Components::CaptureManager::Get().IsCapturing();
    }

    void Install()
    {
        UnloadZonesWithClient();
        HookUnloadZonesWithClient();
        zonesFolder =Components::Actors::GetCharacterZonesFolder();
        HookZonePath();

        // a map change unloads the pack and may unload the animations' zone
        Events::RegisterListener(EventType::PreDemoLoad, [] {
            animationsLoaded = false;
            packZone.clear();
            std::lock_guard lock(packMutex);
            packState = {.generation = packState.generation + 1};
            requestedPack.clear();
            packUnloadRequested = false;
        });

        HookManager::CreateHook(GetGameAddresses().DB_AddXAsset(), (std::uintptr_t)DB_AddXAsset_Hook,
                                &DB_AddXAsset_Trampoline);
        CampaignMaps::Install(zonesFolder);
        // the campaign's and Zombies' weapons given to the player of a live game
        WeaponZones::Install();
        LiveActors::Install();
        LiveGame::Install();
        // the Change Map menu's campaign and Zombies tabs, with the pictures the setup copied beside the zones
        MapMenu::Install(zonesFolder.parent_path() / std::string(IWXMVM::Setup::MENU_PICTURES_FOLDER));
    }
}  // namespace IWXMVM::T5::ContentPacks
