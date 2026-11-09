#include "StdInclude.hpp"
#include "D3D9.hpp"

#include "MinHook.h"

#include "Components/CaptureManager.hpp"
#include "Components/SurfaceDepth.hpp"
#include "Components/TexturePack.hpp"
#include "Events.hpp"
#include "Graphics/Graphics.hpp"
#include "Utilities/PathUtils.hpp"
#include "Mod.hpp"
#include "ReShade.hpp"
#include "UI/UIManager.hpp"
#include "Utilities/HookManager.hpp"

namespace IWXMVM::D3D9
{
    HWND gameWindowHandle = nullptr;
    void* d3d9DeviceVTable[119];
    void* d3d9SwapChainVTable[10];
    void* d3d9VTable[17];
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* depthTexture = nullptr;
    bool foundInterceptedDepthTexture = false;
    std::uint32_t gameWidth = 0;
    std::uint32_t gameHeight = 0;
    std::atomic<std::uint32_t> resetCount = 0;
    // see ReleaseForUnload
    std::atomic<bool> unloadRequested = false;
    std::atomic<bool> releasedForUnload = false;

    typedef HRESULT(__stdcall* EndScene_t)(IDirect3DDevice9* pDevice);
    EndScene_t EndScene;
    EndScene_t ReshadeOriginalEndScene;
    typedef HRESULT(__stdcall* Reset_t)(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters);
    Reset_t Reset;
    typedef HRESULT(__stdcall* Present_t)(IDirect3DDevice9* pDevice, const RECT* pSourceRect, const RECT* pDestRect,
                                          HWND hDestWindowOverride, const RGNDATA* pDirtyRegion, DWORD dwFlags);
    Present_t SwapChainPresent;
    typedef HRESULT(__stdcall* CreateDevice_t)(IDirect3D9* pInterface, UINT Adapter, D3DDEVTYPE DeviceType,
                                               HWND hFocusWindow, DWORD BehaviorFlags,
                                               D3DPRESENT_PARAMETERS* pPresentationParameters,
                                               IDirect3DDevice9** ppReturnedDeviceInterface);
    CreateDevice_t CreateDevice;
    typedef HRESULT(__stdcall* CreateDepthStencilSurface_t)(IDirect3DDevice9* pDevice, UINT Width, UINT Height,
                                                            D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                                            DWORD MultisampleQuality, BOOL Discard,
                                                            IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle);
    CreateDepthStencilSurface_t CreateDepthStencilSurface;
    typedef HRESULT(__stdcall* SetDepthStencilSurface_t)(IDirect3DDevice9* pDevice, IDirect3DSurface9* pNewZStencil);
    SetDepthStencilSurface_t SetDepthStencilSurface;

    std::optional<void*> reshadeEndSceneAddress;
    bool IsReshadePresent()
    {
        return reshadeEndSceneAddress.has_value();
    }

    std::uint32_t GetResetCount()
    {
        return resetCount;
    }

    HRESULT __stdcall CreateDepthStencilSurface_Hook(IDirect3DDevice9* pDevice, UINT Width, UINT Height,
        D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
        DWORD MultisampleQuality, BOOL Discard,
        IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
    {
        if (IsReshadePresent())
        {
			return CreateDepthStencilSurface(pDevice, Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
        }

        HRESULT hr = D3D_OK;

        if (MultiSample == D3DMULTISAMPLE_NONE && Width == gameWidth && Height == gameHeight)
        {
            LOG_DEBUG("Intercepting depth stencil surface creation");

			if (depthTexture)
			{
				depthTexture->Release();
				depthTexture = nullptr;
			}
			hr = device->CreateTexture(Width, Height, 1, D3DUSAGE_DEPTHSTENCIL, static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT,
				&depthTexture, nullptr);
			if (FAILED(hr))
			{
				LOG_ERROR("Failed to create depth texture");
			}

            hr = depthTexture->GetSurfaceLevel(0, ppSurface);
            if (SUCCEEDED(hr))
            {
                LOG_DEBUG("Successfully changed depth stencil format");
            } else
            {
                LOG_DEBUG("Failed to get surface from depth texture");
                return CreateDepthStencilSurface(pDevice, Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
            }

            return hr;
        }

        return CreateDepthStencilSurface(pDevice, Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
    }

    HRESULT __stdcall CreateDevice_Hook(IDirect3D9* pInterface, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
        DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface)
    {
        LOG_DEBUG("CreateDevice called with hwnd {0:x}", (std::uintptr_t)pPresentationParameters->hDeviceWindow);

        if (UI::UIManager::Get().IsInitialized())
        {
            GFX::GraphicsManager::Get().Uninitialize();
            UI::UIManager::Get().ShutdownImGui();
        }

		foundInterceptedDepthTexture = false;

        Components::SurfaceDepth::OnNewDevice();
        Components::TexturePack::OnReset();
        ReShade::OnDeviceCreated();
        HRESULT hr = CreateDevice(pInterface, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
            ppReturnedDeviceInterface);
        if (FAILED(hr))
        {
            return hr;
        }
        ++resetCount;

        gameWidth = pPresentationParameters->BackBufferWidth;
        gameHeight = pPresentationParameters->BackBufferHeight;

        device = *ppReturnedDeviceInterface;
        if (unloadRequested)
            return hr;

        UI::UIManager::Get().Initialize(device, pPresentationParameters->hDeviceWindow);
        GFX::GraphicsManager::Get().Initialize();

        return hr;
    }

    bool CheckForOverlays(std::uintptr_t returnAddress)
    {
        static constexpr std::array overlayNames{
            "gameoverlay",  // Steam
            "discord"       // Discord
        };
        static std::array<std::uintptr_t, std::size(overlayNames)> returnAddresses{};

        for (std::size_t i = 0; i < returnAddresses.size(); ++i)
        {
            if (!returnAddresses[i])
            {
                MEMORY_BASIC_INFORMATION mbi;
                ::VirtualQuery(reinterpret_cast<LPCVOID>(returnAddress), &mbi, sizeof(MEMORY_BASIC_INFORMATION));

                char module[1024];
                ::GetModuleFileName(static_cast<HMODULE>(mbi.AllocationBase), module, sizeof(module));

                if (std::string_view{module}.find(overlayNames[i]) != std::string_view::npos)
                {
                    returnAddresses[i] = returnAddress;
                    return true;
                }
            }
            else if (returnAddresses[i] == returnAddress)
            {
                return true;
            }
        }

        return false;
    }

    // (on the render thread) whether the mod unloads, having let go of the UI and the graphics' resources
    bool ReleaseIfUnloading()
    {
        if (!unloadRequested)
            return false;
        if (!releasedForUnload)
        {
            ReleaseWindowPicture();
            // (ReShade gets the preset's own values back before the mod's events go)
            ReShade::Shutdown();
            Components::SurfaceDepth::Shutdown(GetDevice());
            Components::TexturePack::Shutdown();
            if (UI::UIManager::Get().IsInitialized())
            {
                GFX::GraphicsManager::Get().Uninitialize();
                UI::UIManager::Get().ShutdownImGui();
            }
            releasedForUnload = true;
        }
        return true;
    }

    bool ReleaseForUnload(std::chrono::milliseconds timeout)
    {
        unloadRequested = true;
        const auto until = std::chrono::steady_clock::now() + timeout;
        while (!releasedForUnload && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return releasedForUnload;
    }

    bool capturedAlready = false;
    std::size_t reshadeEndSceneCallCount;
    HRESULT __stdcall EndScene_Hook(IDirect3DDevice9* pDevice)
    {
        if (ReleaseIfUnloading())
            return EndScene(pDevice);

        Components::CaptureManager::Get().OnFrameDrawn();
        const std::uintptr_t returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        if (CheckForOverlays(returnAddress))
        {
            return EndScene(pDevice);
        }

        if (!UI::UIManager::Get().IsInitialized())
        {
            device = pDevice;
            UI::UIManager::Get().Initialize(pDevice);
            GFX::GraphicsManager::Get().Initialize();
        }

        capturedAlready = false;
        if (Components::CaptureManager::Get().IsCapturing())
        {
            if (IsReshadePresent())
            {
                if (Components::CaptureManager::Get().MultiPassEnabled() && !Components::CaptureManager::Get().GetCurrentPass().useReshade)
                {
                    capturedAlready = true;
                    if (Components::CaptureManager::Get().IsFramePrepared())
                    {
                        Components::CaptureManager::Get().CaptureFrame();
                    }

                    Components::CaptureManager::Get().PrepareFrame();
                }
            }
            else
            {
                if (Components::CaptureManager::Get().IsFramePrepared())
                {
                    Components::CaptureManager::Get().CaptureFrame();
                }

                Components::CaptureManager::Get().PrepareFrame();
            }
        }

        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            GFX::GraphicsManager::Get().Render();
        }

        if (!reshadeEndSceneAddress.has_value())
        {
            UI::UIManager::Get().RunImGuiFrame();
        }

        return EndScene(pDevice);
    }

    // This is only called if reshade is present
    HRESULT __stdcall ReshadeOriginalEndScene_Hook(IDirect3DDevice9* pDevice)
    {
        if (reshadeEndSceneCallCount > 0 || ReleaseIfUnloading())
        {
            return ReshadeOriginalEndScene(pDevice);
        }

        if (Components::CaptureManager::Get().IsCapturing() && (!Components::CaptureManager::Get().MultiPassEnabled() || Components::CaptureManager::Get().GetCurrentPass().useReshade) && !capturedAlready)
        {
            if (Components::CaptureManager::Get().IsFramePrepared())
            {
                Components::CaptureManager::Get().CaptureFrame();
            }

            Components::CaptureManager::Get().PrepareFrame();
        }
      
        ++reshadeEndSceneCallCount;

        // (ReShade's own EndScene, which sets the UI up, may not have come yet: a frame without it drew a UI that
        // wasn't there, and the error box stopped the game, when the mod was loaded as the game started drawing)
        if (!UI::UIManager::Get().IsInitialized())
            return ReshadeOriginalEndScene(pDevice);
        UI::UIManager::Get().RunImGuiFrame();
        // (the frame's ReShade values, after the UI's changes and before ReShade draws in Present)
        ReShade::BeforePresent();
        return ReshadeOriginalEndScene(pDevice);
    }

    // the game view's place in the window's client area, as the editor drew it last (empty: the whole window)
    std::mutex gameViewMutex;
    RECT gameViewRect{};

    void SetGameViewRect(const RECT& rect)
    {
        std::lock_guard lock(gameViewMutex);
        gameViewRect = rect;
    }

    // the window's picture as a restart starts, before the game blanks it (see PaintRestart)
    std::mutex windowPictureMutex;
    HBITMAP windowPicture = nullptr;
    std::atomic<bool> windowPictureUpToDate = false;

    // the loading screen's ring as the editor drew it last, and when
    struct RestartRing
    {
        float x = 0.0f;
        float y = 0.0f;
        float radius = 0.0f;
        float thickness = 0.0f;
        std::uint32_t track = 0;
        std::uint32_t arc = 0;
    };
    std::mutex restartRingMutex;
    RestartRing restartRing;
    std::atomic<std::uint64_t> restartRingDrawn = 0;
    std::jthread restartRingPainter;

    void SetRestartRing(float x, float y, float radius, float thickness, std::uint32_t track, std::uint32_t arc)
    {
        std::lock_guard lock(restartRingMutex);
        restartRing = {x, y, radius, thickness, track, arc};
        restartRingDrawn = GetTickCount64();
    }

    // No frames come for some seconds of a restart, and few while ReShade makes its effects, so with none for a moment
    // the ring is painted on the window, on a thread of its own: the render thread is what is held up. It's the same ring
    // by the same clock, and the next frame goes over it. Up to the game's change the picture goes under it, as the
    // reset blanks the window at moments of its own; after that the last frame says more than the picture does.
    void PaintRestartRing(std::stop_token stop, HWND window)
    {
        // (sooner with the picture: it's the same as a frame, and the reset's blank shows at once)
        constexpr std::uint64_t NO_FRAMES_MS = 120;
        constexpr std::uint64_t NO_FRAMES_PICTURE_MS = 40;
        constexpr std::uint8_t SCREEN = 18;  // the loading screen's background
        std::vector<std::uint32_t> pixels;
        while (!stop.stop_requested())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            RestartRing ring;
            {
                std::lock_guard lock(restartRingMutex);
                ring = restartRing;
            }
            const auto now = GetTickCount64();
            if (ring.radius <= 0.0f ||
                now - restartRingDrawn < (windowPictureUpToDate ? NO_FRAMES_PICTURE_MS : NO_FRAMES_MS))
                continue;

            const auto dc = GetDC(window);
            if (!dc)
                continue;
            const auto side = static_cast<int>(std::ceil((ring.radius + ring.thickness) * 2.0f)) + 2;
            pixels.resize(static_cast<std::size_t>(side) * side);
            const auto left = static_cast<int>(std::floor(ring.x)) - side / 2;
            const auto top = static_cast<int>(std::floor(ring.y)) - side / 2;
            if (windowPictureUpToDate)
            {
                RECT client{};
                GetClientRect(window, &client);
                std::lock_guard lock(windowPictureMutex);
                if (windowPicture)
                {
                    const auto memory = CreateCompatibleDC(dc);
                    const auto previous = SelectObject(memory, windowPicture);
                    // (around the ring, which would flicker)
                    ExcludeClipRect(dc, left, top, left + side, top + side);
                    BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
                    SelectClipRgn(dc, nullptr);
                    SelectObject(memory, previous);
                    DeleteDC(memory);
                }
            }
            const auto turn = Components::RestartLogic::GetRingTurn(now);
            const auto mix = [](float from, std::uint32_t color, int shift, float share) {
                return from + (static_cast<float>((color >> shift) & 0xFF) - from) * share;
            };
            for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x)
                {
                    const auto [track, arc] = Components::RestartLogic::GetRingPixel(
                        left + x + 0.5f - ring.x, top + y + 0.5f - ring.y, ring.radius, ring.thickness, turn);
                    std::uint32_t pixel = 0;
                    for (const int shift : {0, 8, 16})
                    {
                        const auto channel = mix(mix(SCREEN, ring.track, shift, track), ring.arc, shift, arc);
                        pixel |= static_cast<std::uint32_t>(channel + 0.5f) << shift;
                    }
                    pixels[static_cast<std::size_t>(y) * side + x] = pixel;
                }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = side;
            info.bmiHeader.biHeight = -side;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(dc, left, top, side, side, 0, 0, 0, side, pixels.data(), &info, DIB_RGB_COLORS);
            ReleaseDC(window, dc);
        }
    }

    void LetGoOfWindowPicture()
    {
        // (stops it and waits for it)
        restartRingPainter = {};
        std::lock_guard lock(windowPictureMutex);
        if (windowPicture)
            DeleteObject(windowPicture);
        windowPicture = nullptr;
    }

    void ReleaseWindowPicture()
    {
        LetGoOfWindowPicture();
        std::lock_guard lock(restartRingMutex);
        restartRing = {};
    }

    void KeepWindowPicture()
    {
        LetGoOfWindowPicture();
        std::lock_guard lock(windowPictureMutex);
        const auto window = FindWindowHandle();
        RECT client{};
        if (!window || !GetClientRect(window, &client))
            return;
        // (from the screen: the window's own DC has no picture of what D3D presents)
        POINT origin{};
        ClientToScreen(window, &origin);
        const auto dc = GetDC(nullptr);
        if (!dc)
            return;
        const auto memory = CreateCompatibleDC(dc);
        windowPicture = CreateCompatibleBitmap(dc, client.right, client.bottom);
        const auto previous = SelectObject(memory, windowPicture);
        BitBlt(memory, 0, 0, client.right, client.bottom, dc, origin.x, origin.y, SRCCOPY);
        SelectObject(memory, previous);
        DeleteDC(memory);
        ReleaseDC(nullptr, dc);
        windowPictureUpToDate = true;
        restartRingPainter = std::jthread(PaintRestartRing, window);
    }

    void OnWindowPictureOutOfDate()
    {
        windowPictureUpToDate = false;
    }

    // The game presents nothing for a few seconds while it loads its renderer again after a restart (a capture's render
    // size, Shadow quality), which leaves the window black, so it puts back the window's picture from before the reset,
    // with the loading screen on it, which the next frame covers again
    void PaintRestart()
    {
        const auto window = FindWindowHandle();
        RECT rect{};
        if (!window || !GetClientRect(window, &rect))
            return;

        const auto dc = GetDC(window);
        if (!dc)
            return;

        std::lock_guard pictureLock(windowPictureMutex);
        if (windowPicture)
        {
            const auto memory = CreateCompatibleDC(dc);
            const auto previous = SelectObject(memory, windowPicture);
            BitBlt(dc, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, previous);
            DeleteDC(memory);
        }
        else
        {
            // (in the game view only, as the editor's own loading screen is)
            std::lock_guard lock(gameViewMutex);
            RECT view{};
            if (!IsRectEmpty(&gameViewRect) && IntersectRect(&view, &rect, &gameViewRect))
                rect = view;
            const auto background = CreateSolidBrush(RGB(18, 18, 18));
            FillRect(dc, &rect, background);
            DeleteObject(background);
        }

        ReleaseDC(window, dc);
    }

    HRESULT __stdcall Reset_Hook(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPresentationParameters)
    {
        if (depthTexture)
        {
            depthTexture->Release();
            depthTexture = nullptr;
            foundInterceptedDepthTexture = false;
        }

        const bool wasUIInitialized = UI::UIManager::Get().IsInitialized();
        if (wasUIInitialized)
		{
			for (const auto& component : UI::UIManager::Get().GetUIComponents())
			{
				component->Release();
			}

			GFX::GraphicsManager::Get().Uninitialize();
			UI::UIManager::Get().ShutdownImGui();
		}

        // the depth stencil the game creates next has the new size, see CreateDepthStencilSurface_Hook
        gameWidth = pPresentationParameters->BackBufferWidth;
        gameHeight = pPresentationParameters->BackBufferHeight;

        Components::SurfaceDepth::OnReset(pDevice);
        Components::TexturePack::OnReset();
        ReShade::OnDeviceReset();
        const bool restarting = Components::CaptureManager::Get().IsRestarting();
        const auto resetStarted = GetTickCount64();
        HRESULT hr = Reset(pDevice, pPresentationParameters);
        ++resetCount;
        LOG_DEBUG("The device's reset to {}x{} took {} ms", gameWidth, gameHeight, GetTickCount64() - resetStarted);

        if (wasUIInitialized)
        {
            UI::UIManager::Get().Initialize(pDevice);
            GFX::GraphicsManager::Get().Initialize();
        }

        if (SUCCEEDED(hr) && restarting)
            PaintRestart();

        return hr;
    }

    HRESULT __stdcall SetDepthStencilSurface_Hook(IDirect3DDevice9* pDevice, IDirect3DSurface9* pNewZStencil)
    {
        HRESULT hr = SetDepthStencilSurface(pDevice, pNewZStencil);
        if (!IsReshadePresent() || foundInterceptedDepthTexture || !pNewZStencil)
        {
            return hr;
        }

        D3DSURFACE_DESC surfaceDesc = {};
        pNewZStencil->GetDesc(&surfaceDesc);

        if (surfaceDesc.MultiSampleType == D3DMULTISAMPLE_NONE && surfaceDesc.Format == static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')))
        {
            foundInterceptedDepthTexture = true;
            LOG_DEBUG("Found intercepted depth texture");

            HRESULT result = pNewZStencil->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&depthTexture));
            if (FAILED(result))
            {
                LOG_ERROR("Failed to get parent texture from intercepted surface");
                return hr;
            }
        }

        return hr;
    }

    HRESULT __stdcall SwapChainPresent_Hook(IDirect3DDevice9* pDevice, const RECT* pSourceRect, const RECT* pDestRect,
                                   HWND hDestWindowOverride, const RGNDATA* pDirtyRegion, DWORD dwFlags)
    {
        reshadeEndSceneCallCount = 0;
        return SwapChainPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);

    }

    void CheckPresenceReshade()
    {
        auto IsReshadeDll = [](const std::filesystem::path& reshadePath) {
            if (!std::filesystem::exists(reshadePath))
            {
                return false;
            }

            bool found = false;
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            if (SUCCEEDED(hr))
            {
                IShellItem2* pShellItem;
                hr = SHCreateItemFromParsingName(reshadePath.wstring().c_str(), nullptr, IID_PPV_ARGS(&pShellItem));
                if (SUCCEEDED(hr))
                {
                    LPWSTR fileDesc = nullptr;
                    hr = pShellItem->GetString(PKEY_FileDescription, &fileDesc);
                    if (SUCCEEDED(hr))
                    {
                        if (std::wstring_view(fileDesc).find(L"ReShade") != std::wstring_view::npos)
                        {
                            found = true;
                        }
                        CoTaskMemFree(fileDesc);
                    }
                    pShellItem->Release();
                }
                CoUninitialize();
            }

            return found;
        };

        auto IsReshadeDllPresent = [&](std::string_view dllName) {
            // Check the copies that are actually loaded first. The main executable isn't always next to the game
            // files ReShade is installed into, e.g. with Plutonium's bootstrapper.
            HMODULE modules[1024];
            DWORD bytesNeeded = 0;
            if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytesNeeded))
            {
                const auto moduleCount = std::min<std::size_t>(bytesNeeded / sizeof(HMODULE), std::size(modules));
                for (std::size_t i = 0; i < moduleCount; ++i)
                {
                    char modulePath[MAX_PATH]{};
                    if (!GetModuleFileNameA(modules[i], modulePath, MAX_PATH))
                        continue;

                    const std::filesystem::path path(modulePath);
                    if (_stricmp(path.filename().string().c_str(), std::string(dllName).c_str()) == 0 &&
                        IsReshadeDll(path))
                    {
                        return true;
                    }
                }
            }

            return IsReshadeDll(std::filesystem::path(PathUtils::GetCurrentGameDirectory()) / dllName);
        };

        bool reshadeFound = IsReshadeDllPresent("d3d9.dll") ||
                            IsReshadeDllPresent("dxgi.dll");

        auto device = Mod::GetGameInterface()->GetGameDevicePtr();
        auto vTable = *reinterpret_cast<void***>(device);
        auto orgEndScene = vTable[42];

        if (reshadeFound && orgEndScene != d3d9DeviceVTable[42])
        {
            LOG_DEBUG("Detected Reshade presence; original EndScene address is {}, Reshade EndScene address is {}.",
                      orgEndScene, d3d9DeviceVTable[42]);

            reshadeEndSceneAddress = std::exchange(d3d9DeviceVTable[42], orgEndScene);
        }
    }

    void FindSwapChain()
    {
        const auto device = Mod::GetGameInterface()->GetGameDevicePtr();

        IDirect3DSwapChain9* pSwapChain = nullptr;
        const HRESULT hr = device->GetSwapChain(0, &pSwapChain);

        if (FAILED(hr) || !pSwapChain)
        {
            throw std::runtime_error("Failed to find D3D9 SwapChain!");
        }
        else
        {
            memcpy(d3d9SwapChainVTable, *(void**)pSwapChain, 10 * sizeof(void*));
            pSwapChain->Release();

            LOG_DEBUG("Found D3D9 SwapChain Present address: {}", d3d9SwapChainVTable[3]);
        }
    }

    void CreateDummyDevice()
    {
        IDirect3D9* d3dObj = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3dObj)
        {
            throw std::runtime_error("Failed to create D3D object");
        }

        IDirect3DDevice9* dummyDevice = nullptr;
        D3DPRESENT_PARAMETERS d3d_params{};

        // Try to create device - will fail if in fullscreen
        HRESULT result = d3dObj->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, NULL,
                                              D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT,
                                              &d3d_params, &dummyDevice);

        // Fail -> death
        if (FAILED(result) || !dummyDevice)
        {
            d3dObj->Release();
            throw std::runtime_error("Failed to create dummy D3D device");
        }

        memcpy(d3d9DeviceVTable, *(void**)dummyDevice, 119 * sizeof(void*));
        memcpy(d3d9VTable, *(void**)d3dObj, 17 * sizeof(void*));

        LOG_DEBUG("Created dummy D3D device");

        CheckPresenceReshade();

        dummyDevice->Release();
        d3dObj->Release();
    }

    void Hook()
    {
        // TODO: move minhook initialization somewhere else
        if (MH_Initialize() != MH_OK)
        {
            throw std::runtime_error("Failed to initialize MinHook");
        }

        HookManager::CreateHook((std::uintptr_t)d3d9DeviceVTable[29], (std::uintptr_t)CreateDepthStencilSurface_Hook,
            (std::uintptr_t*)&CreateDepthStencilSurface);
        HookManager::CreateHook((std::uintptr_t)d3d9VTable[16], (std::uintptr_t)CreateDevice_Hook,
            (std::uintptr_t*)&CreateDevice);
        HookManager::CreateHook((std::uintptr_t)d3d9DeviceVTable[16], (std::uintptr_t)Reset_Hook,
            (std::uintptr_t*)&Reset);
        HookManager::CreateHook((std::uintptr_t)d3d9SwapChainVTable[3], (std::uintptr_t)SwapChainPresent_Hook,
            (std::uintptr_t*)&SwapChainPresent);
        HookManager::CreateHook((std::uintptr_t)d3d9DeviceVTable[42], (std::uintptr_t)EndScene_Hook,
            (std::uintptr_t*)&EndScene);
        HookManager::CreateHook((std::uintptr_t)d3d9DeviceVTable[39], (std::uintptr_t)SetDepthStencilSurface_Hook,
            (std::uintptr_t*)&SetDepthStencilSurface);
        Components::SurfaceDepth::Install(d3d9DeviceVTable);
        Components::TexturePack::Install();
        
        if (reshadeEndSceneAddress.has_value())
        {
            HookManager::CreateHook((std::uintptr_t)reshadeEndSceneAddress.value(),
                                    (std::uintptr_t)ReshadeOriginalEndScene_Hook,
                                    (std::uintptr_t*)&ReshadeOriginalEndScene);
        }
    }

    void Initialize()
    {
        FindSwapChain();
        CreateDummyDevice();
        Hook();
        LOG_DEBUG("Hooked D3D9");

        // the Look tab's ReShade section talks to ReShade as an add-on
        if (IsReshadePresent())
            ReShade::Initialize();

        if (!IsReshadePresent())
        {
            LOG_DEBUG("Triggering vid_restart since Reshade is not present");
            Mod::GetGameInterface()->Vid_Restart();
        }
    }

    HWND FindWindowHandle()
    {
        auto* device = GetDevice();
        if (device == nullptr)
        {
            LOG_CRITICAL("Failed to get the game window");
            return nullptr;
        }

        D3DDEVICE_CREATION_PARAMETERS params{};
        device->GetCreationParameters(&params);

        if (params.hFocusWindow == nullptr)
        {
            LOG_CRITICAL("Failed to get the game window");
            return nullptr;
        }

        return gameWindowHandle = params.hFocusWindow;
    }

    IDirect3DDevice9* GetDevice()
    {
        return device;
    }

    IDirect3DTexture9* GetDepthTexture()
    {
        return depthTexture;
    }

    bool CaptureBackBuffer(IDirect3DTexture9* texture)
    {
        auto device = GetDevice();

        IDirect3DSurface9* RenderTarget = NULL;
        auto result = device->GetRenderTarget(0, &RenderTarget);
        if (FAILED(result))
            return false;

        IDirect3DSurface9* textureSurface;
        result = texture->GetSurfaceLevel(0, &textureSurface);
        if (FAILED(result))
        {
            textureSurface->Release();
            RenderTarget->Release();
            return false;
        }

        result = device->StretchRect(RenderTarget, NULL, textureSurface, NULL, D3DTEXF_LINEAR);
        if (FAILED(result))
        {
            textureSurface->Release();
            RenderTarget->Release();
            return false;
        }

        textureSurface->Release();
        RenderTarget->Release();
        return true;
    }

    bool CreateTexture(IDirect3DTexture9*& texture, ImVec2 size)
    {
        if (texture != NULL)
            texture->Release();

        auto device = D3D9::GetDevice();

        auto result = D3DXCreateTexture(device, (UINT)size.x, (UINT)size.y, D3DX_DEFAULT, D3DUSAGE_RENDERTARGET,
                                        D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &texture);
        if (FAILED(result))
            return false;

        return true;
    }
}  // namespace IWXMVM::D3D9
