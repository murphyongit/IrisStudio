#include "StdInclude.hpp"
#include "CursorGuard.hpp"

#include <intrin.h>

#include "Utilities/HookManager.hpp"

namespace IWXMVM::IW5::CursorGuard
{
    namespace
    {
        std::atomic<bool> editorHasMouse = false;
        std::atomic<bool> reported = false;
        HMODULE ownModule = nullptr;

        HMODULE ModuleOf(void* address)
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCWSTR>(address), &module);
            return module;
        }

        typedef BOOL(WINAPI* SetCursorPos_t)(int x, int y);
        SetCursorPos_t SetCursorPos_Trampoline = nullptr;
        BOOL WINAPI SetCursorPos_Hook(int x, int y)
        {
            if (editorHasMouse.load())
            {
                const auto caller = ModuleOf(_ReturnAddress());
                if (caller != ownModule)
                {
                    if (!reported.exchange(true))
                    {
                        char name[MAX_PATH]{};
                        if (caller)
                            GetModuleFileNameA(caller, name, MAX_PATH);
                        LOG_INFO("Kept the cursor where the editor has it: {} tried to move it",
                                 caller ? name : "code outside any module");
                    }
                    return TRUE;
                }
            }
            return SetCursorPos_Trampoline(x, y);
        }
    }  // namespace

    void SetEditorHasMouse(bool has)
    {
        editorHasMouse.store(has);
    }

    void Install()
    {
        ownModule = ModuleOf(reinterpret_cast<void*>(&SetCursorPos_Hook));
        const auto user32 = GetModuleHandleW(L"user32.dll");
        const auto target = user32 ? GetProcAddress(user32, "SetCursorPos") : nullptr;
        if (!target || !ownModule)
        {
            LOG_WARN("SetCursorPos wasn't found: the cursor isn't guarded");
            return;
        }
        HookManager::CreateHook(reinterpret_cast<std::uintptr_t>(target),
                                reinterpret_cast<std::uintptr_t>(&SetCursorPos_Hook),
                                reinterpret_cast<std::uintptr_t*>(&SetCursorPos_Trampoline));
    }
}  // namespace IWXMVM::IW5::CursorGuard
