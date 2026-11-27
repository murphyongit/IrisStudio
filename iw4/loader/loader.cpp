// IW4iris's way into the game: iw4m.exe asks Windows for DDRAW.dll (two functions, to measure video memory) and
// takes the one in its own folder first. This one passes every DirectDraw function on to Windows' own ddraw.dll
// and, when the program is iw4m.exe, loads IW4iris.dll from the same folder. Nothing else: the mod is IW4iris.dll.
//
// To switch the mod off without deleting anything: put a file named IW4iris.off next to it.
#include <windows.h>

namespace
{
    constexpr int COUNT = 22;
    const char* const NAMES[COUNT] = {
        "AcquireDDThreadLock",    "CompleteCreateSysmemSurface", "D3DParseUnknownCommand", "DDGetAttachedSurfaceLcl",
        "DDInternalLock",         "DDInternalUnlock",            "DSoundHelp",             "DirectDrawCreate",
        "DirectDrawCreateClipper", "DirectDrawCreateEx",         "DirectDrawEnumerateA",   "DirectDrawEnumerateExA",
        "DirectDrawEnumerateExW", "DirectDrawEnumerateW",        "DllCanUnloadNow",        "DllGetClassObject",
        "GetDDSurfaceLocal",      "GetOLEThunkData",             "GetSurfaceFromDC",       "RegisterSpecialCase",
        "ReleaseDDThreadLock",    "SetAppCompatData",
    };

    HMODULE self = nullptr;
    bool resolved = false;
    CRITICAL_SECTION lock;

    // a function Windows' ddraw.dll does not have (it has had all of these since Windows XP): nothing is done
    HRESULT WINAPI Missing()
    {
        return E_NOTIMPL;
    }
}  // namespace

// Where each exported name goes: at first to a piece that finds Windows' own functions (the first time any is
// called, not as this DLL starts), from then on straight to Windows' function of that name.
extern "C" FARPROC iris_ddraw[COUNT];

extern "C" void __cdecl iris_resolve()
{
    EnterCriticalSection(&lock);
    if (!resolved)
    {
        char path[MAX_PATH + 16] = {};
        const UINT length = GetSystemDirectoryA(path, MAX_PATH);
        HMODULE real = nullptr;
        if (length > 0 && length < MAX_PATH)
        {
            lstrcatA(path, "\\ddraw.dll");
            real = LoadLibraryA(path);
        }
        for (int i = 0; i < COUNT; ++i)
        {
            const FARPROC proc = real ? GetProcAddress(real, NAMES[i]) : nullptr;
            iris_ddraw[i] = proc ? proc : reinterpret_cast<FARPROC>(&Missing);
        }
        resolved = true;
    }
    LeaveCriticalSection(&lock);
}

#define IRIS_FORWARD(index, name)                                                                   \
    extern "C" __declspec(naked) void iris_first_##name()                                           \
    {                                                                                               \
        __asm pushad __asm pushfd __asm call iris_resolve __asm popfd __asm popad                   \
        __asm jmp dword ptr [iris_ddraw + index * 4]                                                \
    }                                                                                               \
    extern "C" __declspec(naked) void iris_##name()                                                 \
    {                                                                                               \
        __asm jmp dword ptr [iris_ddraw + index * 4]                                                \
    }

IRIS_FORWARD(0, AcquireDDThreadLock)
IRIS_FORWARD(1, CompleteCreateSysmemSurface)
IRIS_FORWARD(2, D3DParseUnknownCommand)
IRIS_FORWARD(3, DDGetAttachedSurfaceLcl)
IRIS_FORWARD(4, DDInternalLock)
IRIS_FORWARD(5, DDInternalUnlock)
IRIS_FORWARD(6, DSoundHelp)
IRIS_FORWARD(7, DirectDrawCreate)
IRIS_FORWARD(8, DirectDrawCreateClipper)
IRIS_FORWARD(9, DirectDrawCreateEx)
IRIS_FORWARD(10, DirectDrawEnumerateA)
IRIS_FORWARD(11, DirectDrawEnumerateExA)
IRIS_FORWARD(12, DirectDrawEnumerateExW)
IRIS_FORWARD(13, DirectDrawEnumerateW)
IRIS_FORWARD(14, DllCanUnloadNow)
IRIS_FORWARD(15, DllGetClassObject)
IRIS_FORWARD(16, GetDDSurfaceLocal)
IRIS_FORWARD(17, GetOLEThunkData)
IRIS_FORWARD(18, GetSurfaceFromDC)
IRIS_FORWARD(19, RegisterSpecialCase)
IRIS_FORWARD(20, ReleaseDDThreadLock)
IRIS_FORWARD(21, SetAppCompatData)

extern "C" FARPROC iris_ddraw[COUNT] = {
    reinterpret_cast<FARPROC>(&iris_first_AcquireDDThreadLock),
    reinterpret_cast<FARPROC>(&iris_first_CompleteCreateSysmemSurface),
    reinterpret_cast<FARPROC>(&iris_first_D3DParseUnknownCommand),
    reinterpret_cast<FARPROC>(&iris_first_DDGetAttachedSurfaceLcl),
    reinterpret_cast<FARPROC>(&iris_first_DDInternalLock),
    reinterpret_cast<FARPROC>(&iris_first_DDInternalUnlock),
    reinterpret_cast<FARPROC>(&iris_first_DSoundHelp),
    reinterpret_cast<FARPROC>(&iris_first_DirectDrawCreate),
    reinterpret_cast<FARPROC>(&iris_first_DirectDrawCreateClipper),
    reinterpret_cast<FARPROC>(&iris_first_DirectDrawCreateEx),
    reinterpret_cast<FARPROC>(&iris_first_DirectDrawEnumerateA),
    reinterpret_cast<FARPROC>(&iris_first_DirectDrawEnumerateExA),
    reinterpret_cast<FARPROC>(&iris_first_DirectDrawEnumerateExW),
    reinterpret_cast<FARPROC>(&iris_first_DirectDrawEnumerateW),
    reinterpret_cast<FARPROC>(&iris_first_DllCanUnloadNow),
    reinterpret_cast<FARPROC>(&iris_first_DllGetClassObject),
    reinterpret_cast<FARPROC>(&iris_first_GetDDSurfaceLocal),
    reinterpret_cast<FARPROC>(&iris_first_GetOLEThunkData),
    reinterpret_cast<FARPROC>(&iris_first_GetSurfaceFromDC),
    reinterpret_cast<FARPROC>(&iris_first_RegisterSpecialCase),
    reinterpret_cast<FARPROC>(&iris_first_ReleaseDDThreadLock),
    reinterpret_cast<FARPROC>(&iris_first_SetAppCompatData),
};

namespace
{
    bool EndsWith(const char* text, const char* end)
    {
        const int a = lstrlenA(text), b = lstrlenA(end);
        return a >= b && lstrcmpiA(text + a - b, end) == 0;
    }

    // IW4iris.dll is loaded from a thread of its own: a DLL's start-up is no place to load another from
    DWORD WINAPI LoadMod(LPVOID)
    {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(self, path, MAX_PATH))
            return 0;

        char* slash = path + lstrlenA(path);
        while (slash > path && slash[-1] != '\\' && slash[-1] != '/')
            --slash;

        char off[MAX_PATH + 32] = {}, mod[MAX_PATH + 32] = {};
        *slash = 0;
        lstrcpyA(off, path);
        lstrcatA(off, "IW4iris.off");
        lstrcpyA(mod, path);
        lstrcatA(mod, "IW4iris.dll");

        if (GetFileAttributesA(off) != INVALID_FILE_ATTRIBUTES)
            return 0;

        LoadLibraryA(mod);
        return 0;
    }
}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        self = instance;
        InitializeCriticalSection(&lock);
        DisableThreadLibraryCalls(instance);

        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (EndsWith(exe, "\\iw4m.exe"))
        {
            if (const HANDLE thread = CreateThread(nullptr, 0, LoadMod, nullptr, 0, nullptr))
                CloseHandle(thread);
        }
    }
    return TRUE;
}
