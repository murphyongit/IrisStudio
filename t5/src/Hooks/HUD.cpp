#include "StdInclude.hpp"
#include "HUD.hpp"

#include "Mod.hpp"
#include "Components/CaptureManager.hpp"
#include "Utilities/HookManager.hpp"
#include "../Addresses.hpp"
#include "../Functions.hpp"
#include "../Structures.hpp"

#ifdef IWXMVM_T5_HARNESS
#include "../Harness.hpp"
#endif

namespace IWXMVM::T5::Hooks::HUD
{
    bool showPlayerHUD = true;
    bool showScore = true;
    bool showIconsAndText = true;
    bool showBloodOverlay = true;
    bool showShellshock = true;

    // The player's HUD (minimap, score, timer, ammo and the like) is made of menus, which don't read ui_hud_hardcore
    // like IW3's do. The game hides most of them while a demo has its HUD hidden (demo_togglegamehud) by a UI
    // visibility bit they hide with. Those are hidden here, apart from the menu the killfeed is in, which has a toggle
    // of its own, and the dirt and snow on the screen, which aren't HUD. So are the popups (medals, calling cards and
    // the card of the player just killed), which are shown while the game allows popups, the stance, the hints, the
    // messages that slide out and the list of players talking.
    bool IsPlayerHudMenu(const Structures::menuDef_t* menu)
    {
        if (!menu->name)
            return false;

        const std::string_view name = menu->name;
        if (menu->hideBits() & Structures::VISIBILITY_GAME_HUD_HIDDEN)
            return name != "gamemessages" && !name.starts_with("fullscreen_");

        return (menu->showBits & Structures::VISIBILITY_POPUPS) || name == "stance" || name == "centerobituary" ||
               name == "cursorhints" || name.ends_with("hint") || name.ends_with("hints") || name == "talkers" ||
               name == "amitalking" || name.starts_with("gamemessageslideout");
    }

    // the blood while the player is hurt and the damage direction indicators; the red flash and the indicators drawn
    // outside the menus are patched out (see Patches.hpp)
    bool IsBloodOverlayMenu(const Structures::menuDef_t* menu)
    {
        return menu->name && (std::string_view(menu->name) == "low health" ||
                              std::string_view(menu->name) == "directional hit indicator");
    }

    bool IsHiddenMenu(const Structures::menuDef_t* menu)
    {
        if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
            return false;

        return (!showPlayerHUD && IsPlayerHudMenu(menu)) || (!showBloodOverlay && IsBloodOverlayMenu(menu));
    }

    typedef bool(__cdecl* Menu_IsVisible_t)(int localClientNum, void* dc, Structures::menuDef_t* menu);
    Menu_IsVisible_t Menu_IsVisible_Trampoline;
    bool __cdecl Menu_IsVisible_Hook(int localClientNum, void* dc, Structures::menuDef_t* menu)
    {
        const bool visible = !IsHiddenMenu(menu) && Menu_IsVisible_Trampoline(localClientNum, dc, menu);
#ifdef IWXMVM_T5_HARNESS
        Harness::OnMenuChecked(menu, visible);
#endif
        return visible;
    }

    // "Connection Interrupted" and its icon show whenever snapshots stop coming, as while a demo is paused
    bool __cdecl ShouldDrawDisconnect()
    {
        return Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo;
    }

    // takes the local client in eax
    std::uintptr_t CG_DrawDisconnect_Trampoline;
    __declspec(naked) void CG_DrawDisconnect_Hook()
    {
        __asm
        {
            push eax
            call ShouldDrawDisconnect
            mov ecx, eax
            pop eax
            test cl, cl
            jz skip
            jmp dword ptr [CG_DrawDisconnect_Trampoline]
        skip:
            ret
        }
    }

    // Script HUD elements are drawn from what CG_CollectHudElems gathers, by the 2D HUD and by the passes that draw
    // waypoints. Those also draw while cg_draw2D is off (as it is in the mod's cameras), like the skulls over
    // teammates that died. Elements that are hidden are left out of what it gathers.
    int __cdecl FilterHudElems(int count, Structures::hudelem_s** elems)
    {
        static Structures::dvar_t* cg_draw2D = nullptr;
        if (!cg_draw2D)
            cg_draw2D = Functions::FindDvar("cg_draw2D");
        const bool draw2D = !cg_draw2D || cg_draw2D->current.enabled;

        int kept = 0;
        for (int i = 0; i < count; ++i)
        {
            const bool show =
                draw2D && (elems[i]->type == Structures::HE_TYPE_SCORE ? showScore : showIconsAndText);
            if (show)
                elems[kept++] = elems[i];
        }
        return kept;
    }

    // takes the array in edx and returns the count in eax
    std::uintptr_t CG_CollectHudElems_Trampoline;
    __declspec(naked) void CG_CollectHudElems_Hook()
    {
        __asm
        {
            push edx
            call dword ptr [CG_CollectHudElems_Trampoline]
            pop edx
            push edx
            push eax
            call FilterHudElems
            add esp, 8
            ret
        }
    }

    // Plutonium draws "Plutonium T5" in a corner of the screen; it is drawn as always, and the render thread skips it
    // while a capture of a demo runs. Every frame a capture writes is drawn while it runs.
    // Leaving it out of what the game is given to draw instead (R_AddCmdDrawText) made the game freeze or crash.
    constexpr std::string_view WATERMARK = "Plutonium T5";

    // the text follows the fields of the command (see R_AddCmdDrawText)
    constexpr std::size_t DRAW_TEXT_CMD_TEXT = 0x5C;

    typedef void(__cdecl* RB_DrawText2DCmd_t)(const std::uint8_t** execState);
    RB_DrawText2DCmd_t RB_DrawText2DCmd_Trampoline;
    void __cdecl RB_DrawText2DCmd_Hook(const std::uint8_t** execState)
    {
        if (Components::CaptureManager::Get().IsCapturing() &&
            reinterpret_cast<const char*>(*execState + DRAW_TEXT_CMD_TEXT) == WATERMARK)
        {
            // a command's handler moves the execute state on to the next command, by the size the command starts
            // with; the loop that calls it doesn't, so it would run this one again and again
            *execState += *reinterpret_cast<const std::uint16_t*>(*execState);
            return;
        }

        RB_DrawText2DCmd_Trampoline(execState);
    }

    void Install()
    {
        // (the game's own setting until the Look tab gives its own)
        if (const auto cg_drawShellshock = Functions::FindDvar("cg_drawShellshock"))
            showShellshock = cg_drawShellshock->current.enabled;
        HookManager::CreateHook(GetGameAddresses().Menu_IsVisible(), (std::uintptr_t)Menu_IsVisible_Hook,
                                (std::uintptr_t*)&Menu_IsVisible_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_CollectHudElems(), (std::uintptr_t)CG_CollectHudElems_Hook,
                                &CG_CollectHudElems_Trampoline);
        HookManager::CreateHook(GetGameAddresses().CG_DrawDisconnect(), (std::uintptr_t)CG_DrawDisconnect_Hook,
                                &CG_DrawDisconnect_Trampoline);
        HookManager::CreateHook(GetGameAddresses().RB_DrawText2DCmd(), (std::uintptr_t)RB_DrawText2DCmd_Hook,
                                (std::uintptr_t*)&RB_DrawText2DCmd_Trampoline);
    }
}  // namespace IWXMVM::T5::Hooks::HUD
