#include "StdInclude.hpp"
#include "Rendering.hpp"

#include "../CampaignMaps.hpp"
#include "../MapMenu.hpp"
#include "../ContentPacks.hpp"
#include "../LiveActors.hpp"
#include "../LiveGame.hpp"
#include "../PoolGrowth.hpp"
#include "../WeaponZones.hpp"

#include "Components/Rendering.hpp"
#include "Components/Rewinding.hpp"
#include "Utilities/HookManager.hpp"
#include "../Addresses.hpp"
#include "../Structures.hpp"
#include "../Functions.hpp"
#ifdef IWXMVM_T5_HARNESS
#include "../Harness.hpp"
#endif

namespace IWXMVM::T5::Hooks::Rendering
{
    // The passes that leave out the players or the world (see Components::Rendering) skip the surfaces of their
    // materials, the way IW3's binding does. Models use mc/ materials, the world wc/ ones; characters, their arms and
    // the weapons are told apart by name.
    constexpr std::array PLAYER_MATERIALS = {
        "mc/mtl_c_",                 // characters, heads included
        "mc/mtl_viewmodel",          // first person arms
        "mc/mtl_gen_eye",
        "weapon",                    // mtl_weapon_*, mtl_t5_weapon_*, t5_weapon_*
        "mc/mtl_marine_glove",
        "mc/mtl_german_shepherd",    // the dogs killstreak
        "mc/mtl_usa_morphine",       // the syrette of Second Chance
        "mc/mtl_mag_wrap",
        "mc/mtl_main_minigun",
        "mc/mtl_ballistic_knife",
        "mc/mtl_fx_shell",           // casings
        "mc/mtl_fx_shotgun_shell",
        "mc/mtl_fx_bullet_chain",
    };

    // Muzzle flashes belong to the players, as the guns do. The flash of a gun a player holds is made of flame cards
    // (gfx_fxt_fire_flame_vert_*, found by moving the effect materials drawn while a player fires from pass to pass),
    // gfx_fxt_gas_flash is only drawn while players fire, and the first person flash is gfx_fxt_muz_vert_gen.
    constexpr std::array MUZZLE_FLASH_MATERIALS = {"muz", "fire_flame_vert", "gas_flash"};

    // drawn in every pass, as in IW3
    constexpr std::array TRACER_MATERIALS = {"trace"};

    enum class MaterialKind : std::uint8_t
    {
        World,
        Player,
        Always,
    };

    struct Material
    {
        const char* name;
    };

    constexpr std::size_t MAX_SORTED_MATERIALS = 0x1000;

    // per sorted material index, filled in on first use; only the render thread draws
    std::array<const Material*, MAX_SORTED_MATERIALS> classifiedMaterials{};
    std::array<MaterialKind, MAX_SORTED_MATERIALS> materialKinds{};

    bool NameContainsAny(const char* name, const auto& parts)
    {
        return std::any_of(parts.begin(), parts.end(), [&](const char* part) { return strstr(name, part); });
    }

#ifdef IWXMVM_T5_HARNESS
    // 0 = classified by name, else MaterialKind + 1
    std::array<std::atomic<std::uint8_t>, MAX_SORTED_MATERIALS> materialKindOverrides{};

    void OverrideMaterialKind(std::uint32_t sortedIndex, int kind)
    {
        materialKindOverrides[sortedIndex % MAX_SORTED_MATERIALS] = static_cast<std::uint8_t>(kind + 1);
    }
#endif

    MaterialKind GetMaterialKind(std::uint32_t sortedIndex)
    {
#ifdef IWXMVM_T5_HARNESS
        if (const auto kind = materialKindOverrides[sortedIndex].load(); kind != 0)
            return static_cast<MaterialKind>(kind - 1);
#endif

        const auto material = reinterpret_cast<const Material* const*>(GetGameAddresses().rgp_sortedMaterials())[sortedIndex];
        if (!material || !material->name)
            return MaterialKind::Always;

        if (classifiedMaterials[sortedIndex] != material)
        {
            classifiedMaterials[sortedIndex] = material;
            materialKinds[sortedIndex] = NameContainsAny(material->name, TRACER_MATERIALS) ? MaterialKind::Always
                                         : NameContainsAny(material->name, MUZZLE_FLASH_MATERIALS) ||
                                                 NameContainsAny(material->name, PLAYER_MATERIALS)
                                             ? MaterialKind::Player
                                             : MaterialKind::World;
        }
        return materialKinds[sortedIndex];
    }

#ifdef IWXMVM_T5_HARNESS
    std::atomic<bool> recordingDrawnMaterials = false;
    std::array<std::atomic<bool>, MAX_SORTED_MATERIALS> drawnMaterials{};

    void RecordDrawnMaterials(bool record)
    {
        if (record)
        {
            for (auto& drawn : drawnMaterials)
                drawn = false;
        }
        recordingDrawnMaterials = record;
    }

    std::vector<std::uint32_t> GetDrawnMaterials()
    {
        std::vector<std::uint32_t> indices;
        for (std::uint32_t i = 0; i < MAX_SORTED_MATERIALS; i++)
        {
            if (drawnMaterials[i])
                indices.push_back(i);
        }
        return indices;
    }
#endif

    bool __cdecl ShouldSkipDrawSurf(std::uint32_t drawSurfLow, std::uint32_t drawSurfHigh)
    {
        // the material's sorted index is bits 31 to 42 of the sort key
        const auto sortedIndex = ((drawSurfLow >> 31) | (drawSurfHigh << 1)) & (MAX_SORTED_MATERIALS - 1);
#ifdef IWXMVM_T5_HARNESS
        if (recordingDrawnMaterials)
            drawnMaterials[sortedIndex] = true;
#endif

        const auto flags = Components::Rendering::GetRenderingFlags();
        if (flags == Types::RenderingFlags_DrawEverything)
            return false;

        switch (GetMaterialKind(sortedIndex))
        {
            case MaterialKind::Player:
                return (flags & Types::RenderingFlags_DrawPlayers) == 0;
            case MaterialKind::World:
                return (flags & Types::RenderingFlags_DrawWorld) == 0;
            default:
                return false;
        }
    }

    // takes a pointer in eax and the sort key as its last two of five stack arguments
    std::uintptr_t R_SetupDrawSurfMaterial_Trampoline;
    __declspec(naked) void R_SetupDrawSurfMaterial_Hook()
    {
        __asm
        {
            push eax
            push dword ptr [esp + 0x18]
            push dword ptr [esp + 0x18]
            call ShouldSkipDrawSurf
            add esp, 8
            test al, al
            pop eax
            jnz skip_surface
            jmp dword ptr [R_SetupDrawSurfMaterial_Trampoline]
        skip_surface:
            xor eax, eax
            ret
        }
    }

    // While a capture renders at a size larger than the desktop, the window keeps its size and D3D scales the image
    // into it; the game itself refuses window sizes larger than the desktop
    std::atomic<std::int32_t> overrideWidth = 0;
    std::atomic<std::int32_t> overrideHeight = 0;

    // the game window and where it was before the override, set before overrideWidth
    HWND heldWindow = nullptr;
    RECT heldWindowRect{};

    // the override only applies in windowed mode
    std::atomic<bool> fullscreen = false;

    std::atomic<bool> vidRestartRequested = false;

    bool SetRenderSizeOverride(std::optional<std::pair<std::int32_t, std::int32_t>> size, HWND window)
    {
        if (size && fullscreen)
            return false;

        if (size && overrideWidth == 0)
        {
            heldWindow = window;
            GetWindowRect(window, &heldWindowRect);
        }

        overrideWidth = size ? size->first : 0;
        overrideHeight = size ? size->second : 0;
        vidRestartRequested = true;
        return true;
    }

    void SetSunShadowMapSize(std::int32_t size, bool restart)
    {
        const auto constant = reinterpret_cast<std::int32_t*>(GetGameAddresses().sunShadowMapSizeConstant());
        if (*constant != size)
        {
            DWORD protection = 0;
            VirtualProtect(constant, sizeof(*constant), PAGE_EXECUTE_READWRITE, &protection);
            *constant = size;
            VirtualProtect(constant, sizeof(*constant), protection, &protection);
            FlushInstructionCache(GetCurrentProcess(), constant, sizeof(*constant));
        }
        if (restart && GetSunShadowMapSize() != size)
            vidRestartRequested = true;
    }

    std::int32_t GetSunShadowMapSize()
    {
        return *reinterpret_cast<const std::int32_t*>(GetGameAddresses().sunShadowMapSize());
    }

    // what other threads asked the main thread to run (see RunOnMainThread): 0 asked, 1 running, 2 done, -1 given up
    struct MainThreadAction
    {
        std::function<void()> action;
        std::shared_ptr<std::atomic<int>> state;
    };
    std::mutex mainThreadMutex;
    std::vector<MainThreadAction> mainThreadActions;

    std::atomic<std::uint32_t> mainThreadFrames = 0;

    std::uint32_t GetMainThreadFrames()
    {
        return mainThreadFrames.load();
    }

    void RunMainThreadFrame()
    {
        ++mainThreadFrames;
#ifdef IWXMVM_T5_HARNESS
        Harness::RunMainThreadActions();
#endif
        if (vidRestartRequested.exchange(false))
            Functions::Cbuf_AddText("vid_restart");
        PoolGrowth::LogNewStrays();
        CampaignMaps::FinishLoad();
        MapMenu::Frame();
        ContentPacks::RunRequestedLoad();
        ContentPacks::RunCharacterZoneRequests();
        WeaponZones::RunFrame();
        LiveActors::RunFrame();
        LiveGame::RunFrame();
        // a demo left (STOP) can be played again: the Demo tab's PLAY copies it over the copy the game played
        if (!Structures::GetClientConnection()->demoplaying)
            Components::Rewinding::CloseDemoFile();

        std::vector<MainThreadAction> actions;
        {
            std::lock_guard lock(mainThreadMutex);
            actions.swap(mainThreadActions);
        }
        for (auto& [action, state] : actions)
        {
            if (int asked = 0; !state->compare_exchange_strong(asked, 1))
                continue;
            action();
            *state = 2;
        }
    }

    bool RunOnMainThread(std::function<void()> action, std::chrono::milliseconds timeout)
    {
        const auto state = std::make_shared<std::atomic<int>>(0);
        {
            std::lock_guard lock(mainThreadMutex);
            mainThreadActions.push_back({std::move(action), state});
        }
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (*state != 2)
        {
            // given up on unless it has started, which it is then waited for
            if (int asked = 0; std::chrono::steady_clock::now() >= until && state->compare_exchange_strong(asked, -1))
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    void __cdecl OverrideWindowParms(Structures::GfxWindowParms* parms)
    {
        fullscreen = parms->fullscreen;
        if (overrideWidth == 0 || parms->fullscreen)
            return;

        parms->sceneWidth = parms->displayWidth = overrideWidth;
        parms->sceneHeight = parms->displayHeight = overrideHeight;
    }

    // takes the GfxWindowParms in edi
    std::uintptr_t R_SetWndParms_Trampoline;
    __declspec(naked) void R_SetWndParms_Hook()
    {
        __asm
        {
            call R_SetWndParms_Trampoline
            push eax
            push edi
            call OverrideWindowParms
            add esp, 4
            pop eax
            ret
        }
    }

    // The game sizes its window to the render size before and after resetting the device, which would also give the
    // overlay, which initializes again with the device, the size of the clamped window. The window is held instead.
    using SetWindowPos_t = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
    SetWindowPos_t SetWindowPos_Trampoline;
    BOOL WINAPI SetWindowPos_Hook(HWND window, HWND insertAfter, int x, int y, int width, int height, UINT flags)
    {
        if (overrideWidth != 0 && window == heldWindow)
        {
            x = heldWindowRect.left;
            y = heldWindowRect.top;
            width = heldWindowRect.right - heldWindowRect.left;
            height = heldWindowRect.bottom - heldWindowRect.top;
        }
        return SetWindowPos_Trampoline(window, insertAfter, x, y, width, height, flags);
    }

    void Install()
    {
        HookManager::CreateHook(reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowPos")),
                                (std::uintptr_t)SetWindowPos_Hook, (std::uintptr_t*)&SetWindowPos_Trampoline);
        HookManager::CreateHook(GetGameAddresses().R_SetWndParms(), (std::uintptr_t)R_SetWndParms_Hook,
                                &R_SetWndParms_Trampoline);
        HookManager::CreateHook(GetGameAddresses().R_SetupDrawSurfMaterial(),
                                (std::uintptr_t)R_SetupDrawSurfMaterial_Hook, &R_SetupDrawSurfMaterial_Trampoline);
    }
}  // namespace IWXMVM::T5::Hooks::Rendering
