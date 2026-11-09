// IrisStudio.exe, the launcher: a 400 x 400 window without a frame of its own, drawn with
// Direct3D 9 and the ImGui the mod uses, that sets up what the mod needs and starts Plutonium T5 in LAN with the mod.
//
// Usage: IrisStudio.exe [--home <folder>] [--storage <folder>] [--plutonium <folder>] [--palette <id>] [--scale <f>]
//                   [--demo <state>] [--shot <file.png>] [--no-steam] and the tests' options below
//   --home       the launcher's own folder instead of %LOCALAPPDATA%\IrisStudio (launcher.json, T5iris.dll, tools)
//   --storage    the mod's folder the zones and the animation pack go into, instead of Plutonium's
//                storage\t5\iris (tests set up into an empty folder this way)
//   (either folder, when it has the usual name, is first moved there from its old name beside it: bo1mvm and
//   IWXMVM)
//   --plutonium  Plutonium's folder instead of %LOCALAPPDATA%\Plutonium
//   --palette    a palette for this run (graphite, signal, phosphor, olive, crimson, violet)
//   --scale      the UI's scale instead of the monitor's
//   --demo       a state with the mockup's data: first, asking, setup, ready, launching, running, problem, settings,
//                a beta build's key, key-copied, key-noclip, key-other-pc, key-ended, key-invalid, key-valid, and
//                Settings > Texture pack's texture-summary (texture-summary-maps: Maps open), texture-building,
//                texture-done (texture-done-all: every map built), texture-checking, texture-repair,
//                texture-repairing, texture-repaired, texture-import; ready-mods: 20 mods in the Mod row
//   --shot       draws the window without showing it, saves it as a PNG and quits
//   --no-steam   doesn't look for the game in Steam's libraries (to test a machine without Steam)
//   --no-update-check   doesn't ask GitHub for the latest release, whatever Settings say
//   --mod <name|none>   the Mod row's pick for this run (the game started with that mod; none for no mod)
// Tests:
//   --setup [--rebuild] [--download] [--cancel-after <s>]   sets up without a window (what is missing, or all of
//                it with --rebuild; OpenAssetTools and FFmpeg downloaded only with --download), the log in
//                <home>\logs, and quits with 0 when it succeeded
//   --oat-zip <file>   OpenAssetTools from a zip on disk instead of the download
//   --ffmpeg-zip <file>   FFmpeg from a zip on disk (with .../bin/ffmpeg.exe) instead of the download
//   --own-tools  only the launcher's own folder counts for OpenAssetTools and FFmpeg, not setup.ps1's or
//                codmvm's
//   --press <setup|accept|launch|rebuild|remove|paste|copy|texture|repair|mod>   presses that button once the first
//                detection is done (accept: Set up T5iris's summary accepted at once; paste and copy: a beta build's
//                Paste key and Copy; texture: Settings > Texture pack's Open; repair: its Repair once the check has
//                found wrong images; mod: opens the Mod row's list)
//   --beta-key <text>   what Paste key reads instead of the clipboard, in a beta build
//   --no-key-check   a beta build with the test key pair only: no key screen, so a launch reaches the mod's own check
//   --shot-delay <s>   with --shot: saves the window this many seconds after the first detection
//   --migrate    only moves the folders from their old names (as every start does), then quits with 0 unless
//                one stays old (logs\folders.log says why)
//   --build-textures <zone,...> [--texture-scale 2|4] [--texture-limit <n>]   builds a texture pack of those maps
//                without a window (the tools downloaded if missing), at most n textures a
//                map, the log in <home>\logs\textures.log, and quits with 0 when it succeeded
//   --import-textures <folder>   imports a folder of DDS files as Import... does, without a window
//   --check-textures   checks the pack's images of an older build against the game's, without a window, listing the
//                wrong ones in the log
//   --repair-textures   and makes the wrong ones again, the rest of the pack left as it is

#include <Windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <wincodec.h>

#include <d3d9.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <vector>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

#include "App.hpp"
#include "Injector.hpp"
#include "Platform.hpp"
#include "Setup.hpp"
#include "TexturePack.hpp"
#include "Setup/Folders.hpp"
#include "../res/resource.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr float WINDOW_SIZE = 400.0f;

    Launcher::App* app = nullptr;
    IDirect3D9* direct3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    D3DPRESENT_PARAMETERS parameters{};
    bool fontsDirty = false;
    float pendingScale = 0.0f;
    float forcedScale = 0.0f;

    std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR path = nullptr;
        std::filesystem::path result;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
            result = path;
        CoTaskMemFree(path);
        return result;
    }

    float ScaleFor(HWND window)
    {
        if (forcedScale > 0.0f)
            return forcedScale;
        const auto dpi = window ? GetDpiForWindow(window) : GetDpiForSystem();
        return dpi / 96.0f;
    }

    void ResetDevice()
    {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        if (device->Reset(&parameters) == D3DERR_INVALIDCALL)
            IM_ASSERT(0);
        ImGui_ImplDX9_CreateDeviceObjects();
    }

    // the window's border color, and its corners rounded where Windows 11 rounds them
    void SetFrameColors(HWND window)
    {
        const auto& p = app->Palette();
        const DWORD corners = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE*/, &corners, sizeof(corners));
        const COLORREF border = RGB(p.border.R(), p.border.G(), p.border.B());
        DwmSetWindowAttribute(window, 34 /* DWMWA_BORDER_COLOR*/, &border, sizeof(border));
        const COLORREF caption = RGB(p.surface.R(), p.surface.G(), p.surface.B());
        DwmSetWindowAttribute(window, 35 /* DWMWA_CAPTION_COLOR*/, &caption, sizeof(caption));
    }

    LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam))
            return true;

        switch (message)
        {
            case WM_NCHITTEST:
            {
                POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
                ScreenToClient(window, &point);
                if (app && app->IsDragArea(static_cast<float>(point.x), static_cast<float>(point.y)))
                    return HTCAPTION;
                return HTCLIENT;
            }
            case WM_SIZE:
                if (device && wParam != SIZE_MINIMIZED)
                {
                    parameters.BackBufferWidth = LOWORD(lParam);
                    parameters.BackBufferHeight = HIWORD(lParam);
                    ResetDevice();
                }
                return 0;
            case WM_DPICHANGED:
            {
                pendingScale = forcedScale > 0.0f ? forcedScale : HIWORD(wParam) / 96.0f;
                fontsDirty = true;
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_ACTIVATE:
                if (app && LOWORD(wParam) != WA_INACTIVE)
                    app->Activated();
                break;
            case WM_SYSCOMMAND:
                // no menu from Alt
                if ((wParam & 0xFFF0) == SC_KEYMENU)
                    return 0;
                break;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    bool CreateDevice(HWND window)
    {
        direct3d = Direct3DCreate9(D3D_SDK_VERSION);
        if (!direct3d)
            return false;
        parameters = {};
        parameters.Windowed = TRUE;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
        parameters.EnableAutoDepthStencil = FALSE;
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        return direct3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                      D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters,
                                      &device) >= 0 ||
               direct3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                      D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &parameters,
                                      &device) >= 0;
    }

    // the back buffer as a PNG (--shot)
    bool SaveBackBuffer(const std::filesystem::path& file)
    {
        IDirect3DSurface9* backBuffer = nullptr;
        IDirect3DSurface9* copy = nullptr;
        bool saved = false;
        D3DSURFACE_DESC description{};
        if (device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer) < 0)
            return false;
        backBuffer->GetDesc(&description);
        if (device->CreateOffscreenPlainSurface(description.Width, description.Height, description.Format,
                                                D3DPOOL_SYSTEMMEM, &copy, nullptr) >= 0 &&
            device->GetRenderTargetData(backBuffer, copy) >= 0)
        {
            D3DLOCKED_RECT locked{};
            if (copy->LockRect(&locked, nullptr, D3DLOCK_READONLY) >= 0)
            {
                IWICImagingFactory* factory = nullptr;
                IWICBitmapEncoder* encoder = nullptr;
                IWICBitmapFrameEncode* frame = nullptr;
                IWICStream* stream = nullptr;
                if (CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)) >=
                        0 &&
                    factory->CreateStream(&stream) >= 0 &&
                    stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE) >= 0 &&
                    factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder) >= 0 &&
                    encoder->Initialize(stream, WICBitmapEncoderNoCache) >= 0 &&
                    encoder->CreateNewFrame(&frame, nullptr) >= 0 && frame->Initialize(nullptr) >= 0 &&
                    frame->SetSize(description.Width, description.Height) >= 0)
                {
                    // the back buffer's X8R8G8B8 as opaque BGRA, which PNG takes as it is
                    std::vector<BYTE> pixels(static_cast<std::size_t>(description.Width) * description.Height * 4);
                    for (UINT row = 0; row < description.Height; ++row)
                    {
                        const auto* from = static_cast<const BYTE*>(locked.pBits) + row * locked.Pitch;
                        auto* to = pixels.data() + static_cast<std::size_t>(row) * description.Width * 4;
                        std::memcpy(to, from, description.Width * 4);
                        for (UINT x = 0; x < description.Width; ++x)
                            to[x * 4 + 3] = 255;
                    }
                    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                    saved = frame->SetPixelFormat(&format) >= 0 && format == GUID_WICPixelFormat32bppBGRA &&
                            frame->WritePixels(description.Height, description.Width * 4,
                                               static_cast<UINT>(pixels.size()), pixels.data()) >= 0 &&
                            frame->Commit() >= 0 && encoder->Commit() >= 0;
                }
                if (frame)
                    frame->Release();
                if (encoder)
                    encoder->Release();
                if (stream)
                    stream->Release();
                if (factory)
                    factory->Release();
                copy->UnlockRect();
            }
        }
        if (copy)
            copy->Release();
        backBuffer->Release();
        return saved;
    }

    Launcher::Options ParseOptions()
    {
        Launcher::Options options;
        const auto local = KnownFolder(FOLDERID_LocalAppData);
        options.home = local / IWXMVM::Setup::LAUNCHER_FOLDER;
        options.plutonium = local / "Plutonium";

        int count = 0;
        const auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        auto text = [](const wchar_t* argument) {
            const auto path = std::filesystem::path(argument).u8string();
            return std::string(path.begin(), path.end());
        };
        for (int i = 1; arguments && i < count; ++i)
        {
            const std::wstring name = arguments[i];
            const wchar_t* value = i + 1 < count ? arguments[i + 1] : L"";
            if (name == L"--no-steam" || name == L"--setup" || name == L"--rebuild" || name == L"--download" ||
                name == L"--own-tools" || name == L"--no-update-check" || name == L"--migrate" ||
                name == L"--no-key-check" || name == L"--check-textures" || name == L"--repair-textures")
            {
                (name == L"--no-steam"          ? options.noSteam
                 : name == L"--setup"           ? options.headlessSetup
                 : name == L"--rebuild"         ? options.rebuild
                 : name == L"--download"        ? options.download
                 : name == L"--own-tools"       ? options.ownTools
                 : name == L"--migrate"         ? options.migrateOnly
                 : name == L"--no-key-check"    ? options.noKeyCheck
                 : name == L"--check-textures"  ? options.checkTextures
                 : name == L"--repair-textures" ? options.repairTextures
                                                : options.noUpdateCheck) = true;
                continue;
            }
            if (name == L"--home")
                options.home = value;
            else if (name == L"--storage")
                options.storage = value;
            else if (name == L"--plutonium")
                options.plutonium = value;
            else if (name == L"--palette")
                options.palette = text(value);
            else if (name == L"--scale")
                options.scale = std::wcstof(value, nullptr);
            else if (name == L"--demo")
                options.demo = text(value);
            else if (name == L"--shot")
                options.shot = value;
            else if (name == L"--oat-zip")
                options.oatZip = value;
            else if (name == L"--ffmpeg-zip")
                options.ffmpegZip = value;
            else if (name == L"--cancel-after")
                options.cancelAfter = std::wcstof(value, nullptr);
            else if (name == L"--press")
                options.press = text(value);
            else if (name == L"--mod")
                options.mod = text(value) == "none" ? std::string() : text(value);
            else if (name == L"--shot-delay")
                options.shotDelay = std::wcstof(value, nullptr);
            else if (name == L"--beta-key")
                options.betaKey = text(value);
            else if (name == L"--build-textures")
                options.buildTextures = text(value);
            else if (name == L"--texture-scale")
                options.textureScale = static_cast<std::uint32_t>(std::wcstoul(value, nullptr, 10));
            else if (name == L"--import-textures")
                options.importTextures = value;
            else if (name == L"--texture-limit")
                options.textureLimit = std::wcstoul(value, nullptr, 10);
            else
                continue;
            ++i;
        }
        LocalFree(arguments);

        if (options.storage.empty())
            options.storage = options.plutonium / "storage" / "t5" / IWXMVM::Setup::MOD_FOLDER;
        return options;
    }

    // The launcher's folder and the mod's, moved from their names before Iris Studio when they still have them
    // (Setup/Folders.hpp): each that has the new name with a folder of the old name beside it, the mod's only while the
    // game isn't running (the mod moves it itself as it loads). Each move goes into logs\folders.log of the launcher's
    // folder; false when a folder couldn't be moved and the old one is used.
    bool MoveFolders(Launcher::Options& options)
    {
        namespace Setup = IWXMVM::Setup;
        auto named = [](const std::filesystem::path& folder, std::string_view name) {
            return _wcsicmp(folder.filename().c_str(), std::filesystem::path(name).c_str()) == 0;
        };
        const std::vector<Setup::FolderRename> renames = {
            {options.home.parent_path() / Setup::OLD_LAUNCHER_FOLDER, options.home},
            {options.storage.parent_path() / Setup::OLD_MOD_FOLDER, options.storage}};
        std::vector<std::string> lines;
        bool moved = true;
        for (std::size_t i = 0; i < renames.size(); ++i)
        {
            const auto& rename = renames[i];
            auto& folder = i == 0 ? options.home : options.storage;
            if (!named(rename.to, i == 0 ? Setup::LAUNCHER_FOLDER : Setup::MOD_FOLDER))
                continue;
            std::error_code error;
            if (i == 1 && Injector::FindGameWindow() && !std::filesystem::exists(rename.to, error) &&
                std::filesystem::is_directory(rename.from, error))
            {
                folder = rename.from;
                lines.push_back("the game is running, so " + Launcher::Platform::Utf8(rename.from) +
                                " stays until the mod moves it");
                moved = false;
                continue;
            }
            const auto move = Setup::MoveFolder(rename, renames);
            folder = move.folder;
            if (move.moved)
                lines.push_back(std::format("moved {} to {}, the paths in {} files rewritten",
                                            Launcher::Platform::Utf8(rename.from), Launcher::Platform::Utf8(rename.to),
                                            move.rewritten.size()));
            if (!move.problem.empty())
                lines.push_back(move.problem);
            moved = moved && (move.moved || move.folder == rename.to);
        }
        if (!lines.empty())
        {
            SYSTEMTIME time{};
            GetLocalTime(&time);
            std::error_code error;
            std::filesystem::create_directories(options.home / "logs", error);
            std::ofstream log(options.home / "logs" / "folders.log", std::ios::app);
            for (const auto& line : lines)
                log << std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} {}\n", time.wYear, time.wMonth, time.wDay,
                                   time.wHour, time.wMinute, time.wSecond, line);
        }
        return moved;
    }
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto options = ParseOptions();
    const bool foldersMoved = MoveFolders(options);
    if (options.migrateOnly)
    {
        CoUninitialize();
        return foldersMoved ? 0 : 1;
    }
    if (options.headlessSetup)
    {
        const int result = Launcher::RunSetup(options);
        CoUninitialize();
        return result;
    }
    if (!options.buildTextures.empty() || !options.importTextures.empty() || options.checkTextures ||
        options.repairTextures)
    {
        const int result = Launcher::RunTextureBuild(options);
        CoUninitialize();
        return result;
    }
    forcedScale = options.scale;
    const auto shotFile = options.shot;
    const bool shot = !shotFile.empty();
    int exitCode = 0;

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_LAUNCHER));
    windowClass.lpszClassName = L"IrisStudio";
    RegisterClassExW(&windowClass);

    // centered on the monitor of the mouse, at its scale
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    UINT dpiX = 96, dpiY = 96;
    if (const auto shcore = LoadLibraryW(L"shcore.dll"))
    {
        using GetDpiForMonitor_t = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (const auto getDpi = reinterpret_cast<GetDpiForMonitor_t>(GetProcAddress(shcore, "GetDpiForMonitor")))
            getDpi(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), 0, &dpiX, &dpiY);
    }
    float scale = forcedScale > 0.0f ? forcedScale : dpiX / 96.0f;
    const int size = static_cast<int>(std::round(WINDOW_SIZE * scale));
    const auto& work = monitor.rcWork;
    const HWND window = CreateWindowExW(
        0, windowClass.lpszClassName, L"Iris Studio", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
        shot ? -32000 : work.left + (work.right - work.left - size) / 2,
        shot ? -32000 : work.top + (work.bottom - work.top - size) / 2, size, size, nullptr, nullptr, instance, nullptr);

    if (!CreateDevice(window))
    {
        MessageBoxW(nullptr, L"Direct3D 9 couldn't be started.", L"Iris Studio", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX9_Init(device);

    Launcher::App launcher(std::move(options));
    app = &launcher;
    launcher.LoadFonts(scale);
    SetFrameColors(window);

    if (!shot)
    {
        ShowWindow(window, showCommand);
        UpdateWindow(window);
    }

    int framesToDraw = 3;  // frames to draw after input, so hover and focus settle
    int shotFrames = 0;
    std::string paletteShown(launcher.Palette().id);
    bool running = true;
    while (running)
    {
        if (!shot && framesToDraw <= 0 && !launcher.Animating())
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);

        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                running = false;
            framesToDraw = 3;
        }
        if (!running)
            break;

        if (fontsDirty)
        {
            fontsDirty = false;
            ImGui_ImplDX9_InvalidateDeviceObjects();
            launcher.LoadFonts(pendingScale);
            ImGui_ImplDX9_CreateDeviceObjects();
        }

        RECT client{};
        GetClientRect(window, &client);
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        launcher.Frame(static_cast<float>(client.right), static_cast<float>(client.bottom));
        ImGui::EndFrame();
        --framesToDraw;

        if (launcher.WantsClose())
            DestroyWindow(window);
        if (launcher.TakeMinimize())
            ShowWindow(window, SW_MINIMIZE);
        if (launcher.Palette().id != paletteShown)
        {
            paletteShown = launcher.Palette().id;
            SetFrameColors(window);
        }

        const auto& p = launcher.Palette();
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(p.bg.R(), p.bg.G(), p.bg.B()), 1.0f, 0);
        if (device->BeginScene() >= 0)
        {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
        }

        // (the first detection runs on a thread: up to 30 s for it)
        if (shot && ++shotFrames >= 4 && (launcher.Settled() || shotFrames > 1800) &&
            launcher.SettledFor() >= launcher.ShotDelay())
        {
            exitCode = SaveBackBuffer(shotFile) ? 0 : 1;
            break;
        }

        const auto result = device->Present(nullptr, nullptr, nullptr, nullptr);
        if (result == D3DERR_DEVICELOST && device->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
            ResetDevice();
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    app = nullptr;
    if (device)
        device->Release();
    if (direct3d)
        direct3d->Release();
    if (IsWindow(window))
        DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    CoUninitialize();
    return exitCode;
}
