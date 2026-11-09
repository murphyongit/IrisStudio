#include "StdInclude.hpp"
#include "CaptureManager.hpp"

#include "Mod.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Components/QualityLogic.hpp"
#include "Components/Rewinding.hpp"
#include "Components/Playback.hpp"
#include "Components/SurfaceDepth.hpp"
#include "Components/TexturePack.hpp"
#include "Graphics/Graphics.hpp"
#include "Utilities/PathUtils.hpp"
#include "Setup/Folders.hpp"
#include "UI/Toasts.hpp"
#include "D3D9.hpp"
#include "Events.hpp"
#include "ReShade.hpp"

namespace IWXMVM::Components
{
    std::string_view CaptureManager::GetOutputFormatLabel(OutputFormat outputFormat)
    {
        switch (outputFormat)
        {
            case OutputFormat::Video:
                return "Video";
            case OutputFormat::CameraData:
                return "Camera Data";
            case OutputFormat::ImageSequence:
                return "Image Sequence";
            default:
                return "Unknown Output Format";
        }
    }

    std::string_view CaptureManager::GetVideoCodecLabel(VideoCodec codec)
    {
        switch (codec)
        {
            case VideoCodec::Prores4444XQ:
                return "Prores 4444 XQ";
            case VideoCodec::Prores4444:
                return "Prores 4444";
            case VideoCodec::Prores422HQ:
                return "Prores 422 HQ";
            case VideoCodec::Prores422:
                return "Prores 422";
            case VideoCodec::Prores422LT:
                return "Prores 422 LT";
            default:
                return "Unknown Video Codec";
        }
    }

    void CaptureManager::Initialize()
    {
        // Set r_smp_backend to 0. 
        // This should ensure that there are no separate threads for game logic and rendering
        // which frees us from having to do synchronizations for our recording code.
        auto r_smp_backend = Mod::GetGameInterface()->GetDvar("r_smp_backend");
        if (!r_smp_backend.has_value())
        {
            LOG_ERROR("Could not set r_smp_backend; dvar not found");
        }
        else
        {
            r_smp_backend.value().value->int32 = false;
        }

        IDirect3DDevice9* device = D3D9::GetDevice();

        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        {
            LOG_ERROR("Failed to capture backbuffer. Capture resolution not found.");
            return;
        }

        D3DSURFACE_DESC bbDesc = {};
        if (FAILED(backBuffer->GetDesc(&bbDesc)))
        {
            LOG_ERROR("Failed to get backbuffer description. Capture resolution not found.");
            return;
        }

        backBuffer->Release();
        backBuffer = nullptr;

        const Resolution backBufferResolution = {
            static_cast<std::int32_t>(bbDesc.Width),
            static_cast<std::int32_t>(bbDesc.Height),
        };

        // The UI initializes again whenever the game resets its device, as it does when switching render sizes
        // for a capture, so the settings are kept from then on. The back buffer only is the window's size when no
        // capture renders at another one.
        if (initialized)
        {
            if (!switchingRenderSize && !switchingBack && !renderSizeChanged && backBufferResolution != gameResolution)
            {
                if (!captureSettings.renderResolution && captureSettings.resolution == gameResolution)
                    captureSettings.resolution = backBufferResolution;
                gameResolution = backBufferResolution;
            }
            return;
        }
        initialized = true;
        gameResolution = backBufferResolution;

        captureSettings = {
            0,
            0,
            OutputFormat::Video, 
            VideoCodec::Prores4444,
            gameResolution,
            250
        };

        auto& outputDirectory = PreferencesConfiguration::Get().captureOutputDirectory;
        if (outputDirectory.empty())
        {
            outputDirectory = PathUtils::GetModPath() / "recordings";
        }

        Events::RegisterListener(EventType::OnDemoBoundsDetermined, [&]() {
            if (captureSettings.startTick == 0 || captureSettings.endTick == 0)
            {
                auto endTick = Mod::GetGameInterface()->GetDemoInfo().endTick;
                captureSettings.startTick = static_cast<int32_t>(endTick * 0.1);
                captureSettings.endTick = static_cast<int32_t>(endTick * 0.9);
            }
        });

        Events::RegisterListener(EventType::OnFrame, [&]() { OnRenderFrame(); });
    }

    void CaptureManager::CaptureFrame()
    {
        framePrepared = false;

        // before the frames of the rewind it starts, which would be followed by the last second again
        if (demoEnded.exchange(false))
        {
            StopEarly(CaptureWatch::Stop::DemoEnded);
            return;
        }

        // the frames of a rewind or a skip, like the one to the start tick, aren't captured, and with a render thread
        // neither are the frames the game built before the capture or during the rewind (see OnFrameHandedOver)
        if (reportsFrameHandOver ? drawnFrame < 0 : Rewinding::IsRewinding() || holdingStart)
            return;

        const auto passIndex = GetCurrentPassIndex();
        // (whether ReShade drew its effects in the frame captured now: not while it loads them, see RestartLogic)
        const auto drewEffects = ReShade::TakeDrewEffects();
        if (expectsReShade && !drewEffects && (!MultiPassEnabled() || captureSettings.passes[passIndex].useReshade))
            ++framesWithoutReShade;

        FILE* outputPipe = pipe;
        if (MultiPassEnabled())
        {
            GFX::GraphicsManager::Get().DrawShaderForPassIndex(passIndex);

            outputPipe = captureSettings.passes[passIndex].pipe;
        }

        IDirect3DDevice9* device = D3D9::GetDevice();

        if (FAILED(device->StretchRect(backBuffer, NULL, downsampledRenderTarget, NULL, D3DTEXF_NONE)))
        {
            LOG_ERROR("Failed to copy data from backbuffer to render target");
            StopCapture();
            return;
        }

        if (FAILED(device->GetRenderTargetData(downsampledRenderTarget, tempSurface)))
        {
            LOG_ERROR("Failed copy render target data to surface");
            StopCapture();
            return;
        }

        D3DLOCKED_RECT lockedRect = {};
        if (FAILED(tempSurface->LockRect(&lockedRect, nullptr, 0)))
        {
            LOG_ERROR("Failed to lock surface");
            StopCapture();
            return;
        }

        const auto surfaceByteSize = screenDimensions.width * screenDimensions.height * 4;
        std::fwrite(lockedRect.pBits, surfaceByteSize, 1, outputPipe);

        if (capturedFrameCount == 0)
        {
            const auto pack = TexturePack::GetStats();
            firstFrame = {.packSwaps = pack.swaps,
                          .packLeft = pack.drawnLeft,
                          .surfaceDraws = SurfaceDepth::GetStats().mapDraws,
                          .surfaceLeft = SurfaceDepth::GetLoad().drawn};
        }
        capturedFrameCount++;

        if (FAILED(tempSurface->UnlockRect()))
        {
            LOG_ERROR("Failed to unlock surface");
            StopCapture();
            return;
        }

        // a multi-pass capture ends after its last pass, so that every pass gets the same frames
        const auto wroteAllPasses = !MultiPassEnabled() || passIndex == captureSettings.passes.size() - 1;
        const auto currentTick = Playback::GetTimelineTick();
        if (!Rewinding::IsRewinding() && currentTick > captureSettings.endTick && wroteAllPasses)
        {
            StopCapture();
            return;
        }

        const auto stallFrames = CaptureWatch::GetStallFrames(captureSettings.framerate, captureSettings.passes.size());
        if (const auto stop = CaptureWatch::Advance(watch, currentTick, Playback::GetTimelineEnd(), stallFrames);
            stop != CaptureWatch::Stop::None)
            StopEarly(stop);
    }

    void CaptureManager::StopEarly(CaptureWatch::Stop stop)
    {
        const auto reason = CaptureWatch::Describe(stop, UI::Toasts::FormatTime(Playback::GetTimelineTick()));
        LOG_WARN("{0} (end tick {1})", reason, captureSettings.endTick);
        StopCapture();
        UI::Toasts::Show(reason);
    }

    void CaptureManager::OnDemoEnd()
    {
        if (isCapturing)
            demoEnded = true;
    }

    void CaptureManager::PrepareFrame()
    {
        if (!isCapturing.load())
        	return;

        // with a render thread the passes are set when a frame is built and handed over, see OnFrameHandedOver
        if (MultiPassEnabled() && !reportsFrameHandOver)
            Rendering::SetVisibleElements(captureSettings.passes[GetCurrentPassIndex()].elements);

        framePrepared = true;
    }

    std::size_t CaptureManager::GetCurrentPassIndex() const
    {
        if (!MultiPassEnabled())
            return 0;

        const auto frame = reportsFrameHandOver ? std::max(drawnFrame, 0) : capturedFrameCount;
        return static_cast<std::size_t>(frame) % captureSettings.passes.size();
    }

    void CaptureManager::OnFrameHandedOver()
    {
        reportsFrameHandOver = true;

        const auto frame = isCapturing && builtFrameGeneration == captureGeneration ? builtFrame : -1;
        handedOverFrame = frame;
        handedOverTick = Playback::GetTimelineTick();

        // the render thread waits for this frame now, so what only it reads, like the materials it skips, is set for
        // the frame's pass
        if (frame >= 0 && MultiPassEnabled())
            Rendering::SetVisibleElements(
                captureSettings.passes[static_cast<std::size_t>(frame) % captureSettings.passes.size()].elements);
    }

    void CaptureManager::OnFrameDrawn()
    {
        drawnFrame = handedOverFrame;
        drawnTick = handedOverTick.load();
    }

    std::uint32_t CaptureManager::GetDrawnTick() const
    {
        return reportsFrameHandOver ? drawnTick.load() : Playback::GetTimelineTick();
    }

    std::optional<Resolution> GetBackBufferSize()
    {
        IDirect3DSurface9* surface = nullptr;
        if (FAILED(D3D9::GetDevice()->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)))
            return std::nullopt;

        D3DSURFACE_DESC desc = {};
        const auto result = surface->GetDesc(&desc);
        surface->Release();
        if (FAILED(result))
            return std::nullopt;
        return Resolution{static_cast<std::int32_t>(desc.Width), static_cast<std::int32_t>(desc.Height)};
    }

    std::vector<Resolution> CaptureManager::GetRenderResolutions() const
    {
        std::vector<Resolution> resolutions;
        if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_RenderSize))
            return resolutions;

        // (larger either way: a window of a 2,560 x 1,440 screen is shorter, and rendering at 1,440p is still more)
        for (const auto resolution : {Resolution{1920, 1080}, Resolution{2560, 1440}, Resolution{3840, 2160}})
        {
            if (resolution.width > gameResolution.width || resolution.height > gameResolution.height)
                resolutions.push_back(resolution);
        }
        return resolutions;
    }

    void CaptureManager::OnRenderFrame()
    {
        if (restart)
            AdvanceRestart();
    }

    void CaptureManager::StartRestart(RestartLogic::Kind kind)
    {
        restart = RestartLogic::Start(kind, GetTickCount64(), D3D9::GetResetCount());
        restart->reshadeTimeout = reshadeTimeout.value_or(RestartLogic::RESHADE_TIMEOUT_MS);
        restart->holdTimeout = holdTimeout.value_or(RestartLogic::HOLD_TIMEOUT_MS);
        std::lock_guard lock(restartsMutex);
        restartScreen = RestartLogic::GetScreen(*restart, RestartLogic::Step::Held, restart->created);
    }

    bool CaptureManager::AskForRestart(RestartLogic::Kind kind)
    {
        switch (kind)
        {
            case RestartLogic::Kind::Capture:
                if (!Mod::GetGameInterface()->SetRenderSize(std::make_pair(switchTarget.width, switchTarget.height)))
                {
                    LOG_ERROR("The game can't render at {0}", switchTarget.ToString());
                    return false;
                }
                LOG_INFO("Switching to {0} for the capture", switchTarget.ToString());
                return true;
            case RestartLogic::Kind::Back:
                return Mod::GetGameInterface()->SetRenderSize(std::nullopt);
            default:
                return Mod::GetGameInterface()->SetSunShadowMapSize(shadowSize, true);
        }
    }

    void CaptureManager::CancelRestart()
    {
        restart.reset();
        D3D9::ReleaseWindowPicture();
        switchingRenderSize = false;
        switchingBack = false;
        switchingShadows = false;
        renderSizeChanged = false;
    }

    void CaptureManager::AdvanceRestart()
    {
        using RestartLogic::Kind;
        using RestartLogic::Step;
        const auto kind = restart->kind;
        const auto changed = kind == Kind::Shadows ? Mod::GetGameInterface()->GetSunShadowMapSize() == shadowSize
                                                   : GetBackBufferSize() == switchTarget;
        const auto reload = ReShade::GetReload();
        const auto now = GetTickCount64();
        const auto step = RestartLogic::Advance(*restart, now,
                                                {.resets = D3D9::GetResetCount(),
                                                 .changed = changed,
                                                 .moving = Rewinding::IsRewinding(),
                                                 .reshade = {.present = reload.present,
                                                             .effectsOn = reload.effectsOn,
                                                             .loading = reload.loading,
                                                             .framesSince = reload.framesSince},
                                                 .pack = TexturePack::GetLoad(),
                                                 .surface = SurfaceDepth::GetLoad()});
        {
            std::lock_guard lock(restartsMutex);
            restartScreen = RestartLogic::GetScreen(*restart, step, now);
        }
        if (step == Step::Ask)
        {
            if (restart->heldMs)
                LOG_INFO("ReShade loaded its effects: the renderer restarts after {} ms", restart->heldMs);
            // (the loading screen has been drawn by now, so the picture has it)
            D3D9::KeepWindowPicture();
            if (!AskForRestart(kind))
                CancelRestart();
            return;
        }
        if (restart->gameDone)
            D3D9::OnWindowPictureOutOfDate();
        if (step == Step::AtStart)
        {
            // what the first frame draws is what Pack and Surface depth have to have, so the capture goes to its start
            // tick now; still switching until they do. (A file that won't open ends it here and switches back.)
            expectsReShade = ReShade::IsDrawingEffects();
            holdingStart = true;
            BeginCapture();
            return;
        }
        if (step != Step::Done && step != Step::GameTimedOut)
            return;

        const auto ended = *restart;
        restart.reset();
        D3D9::ReleaseWindowPicture();
        RecordRestart(ended, step);
        switch (kind)
        {
            case Kind::Capture:
                if (step == Step::GameTimedOut)
                {
                    LOG_ERROR("The game didn't switch to {0} for the capture", switchTarget.ToString());
                    switchingRenderSize = false;
                    StopCapture();
                    break;
                }
                holdingStart = false;
                // still switching until the capture has started, for whoever waits for it on another thread
                switchingRenderSize = false;
                for (const auto& [load, name] : {std::pair(ended.pack, "Pack"), std::pair(ended.surface, "Surface depth")})
                    if (load.timedOut)
                        UI::Toasts::Show(RestartLogic::DescribeNotReady(name));
                break;
            case Kind::Back:
                switchingBack = false;
                break;
            case Kind::Shadows:
                switchingShadows = false;
                break;
        }
    }

    void CaptureManager::RecordRestart(const RestartLogic::Wait& wait, RestartLogic::Step end)
    {
        using RestartLogic::Step;
        const auto now = GetTickCount64();
        const auto gameDone = wait.gameDone.value_or(now);
        const auto took = [](const RestartLogic::LoadWait& load) {
            return load.on && load.started && load.done ? static_cast<std::uint32_t>(*load.done - *load.started) : 0;
        };
        RestartRecord record{
            .kind = wait.kind,
            .label = GetRestartLabel(),
            .end = end,
            .heldMs = wait.heldMs,
            .gameMs = static_cast<std::uint32_t>(gameDone - wait.started),
            .reshadeMs = static_cast<std::uint32_t>(wait.reshadeDone.value_or(gameDone) - gameDone),
            .packMs = took(wait.pack),
            .surfaceMs = took(wait.surface),
            .waitedForReShade = wait.waitsForReShade,
            .waitedForPack = wait.pack.on,
            .waitedForSurface = wait.surface.on,
            .reshadeTimedOut = wait.reshadeTimedOut,
            .packTimedOut = wait.pack.timedOut,
            .surfaceTimedOut = wait.surface.timedOut,
            .screenFrames = wait.screenFrames,
        };
        const auto describe = [](bool waited, bool timedOut, std::string_view name, std::uint32_t ms) {
            return !waited    ? std::string{}
                   : timedOut ? std::format(", {} out of time after {} ms", name, ms)
                              : std::format(", {} {} ms", name, ms);
        };
        const auto waits = describe(wait.waitsForReShade, wait.reshadeTimedOut, "ReShade", record.reshadeMs) +
                           describe(wait.pack.on, wait.pack.timedOut, "Pack", record.packMs) +
                           describe(wait.surface.on, wait.surface.timedOut, "Surface depth", record.surfaceMs);
        const auto held = wait.heldMs ? std::format("held back {} ms for ReShade, ", wait.heldMs) : "";
        if (end == Step::Done)
            LOG_INFO("Restarted the renderer ({}): {}the game {} ms{}, the screen drawn in {} frames", record.label, held,
                     record.gameMs, waits, record.screenFrames);
        else
            LOG_WARN("The renderer's restart ({}) didn't finish in {} s", record.label,
                     RestartLogic::GAME_TIMEOUT_MS / 1000);

        std::lock_guard lock(restartsMutex);
        restarts.push_back(std::move(record));
        if (restarts.size() > 8)
            restarts.erase(restarts.begin());
    }

    std::vector<CaptureManager::RestartRecord> CaptureManager::GetRestarts() const
    {
        std::lock_guard lock(restartsMutex);
        return restarts;
    }

    void CaptureManager::SetRestartTimeouts(std::optional<std::uint64_t> reshadeMs, std::optional<std::uint64_t> holdMs)
    {
        reshadeTimeout = reshadeMs;
        holdTimeout = holdMs;
    }

    RestartLogic::Screen CaptureManager::GetRestartScreen() const
    {
        std::lock_guard lock(restartsMutex);
        return restartScreen;
    }

    std::string CaptureManager::GetRestartLabel() const
    {
        if (switchingShadows)
            return RestartLogic::GetLabel(RestartLogic::Kind::Shadows, shadowName);
        return RestartLogic::GetLabel(
            switchTarget == gameResolution ? RestartLogic::Kind::Back : RestartLogic::Kind::Capture,
            switchTarget.ToString());
    }

    void CaptureManager::OnRestartScreenDrawn()
    {
        if (restart)
            ++restart->screenFrames;
    }

    void CaptureManager::RestartForShadows(std::int32_t size, std::string_view name)
    {
        if (IsRestarting() || isCapturing)
            return;

        // (before the restart is asked for, whose reset it waits for)
        shadowSize = size;
        shadowName = name;
        StartRestart(RestartLogic::Kind::Shadows);
        switchingShadows = true;
    }

    int32_t CaptureManager::OnGameFrame()
    {
        const auto frameTime = 1000 / GetCaptureSettings().framerate;

        // no frames are captured meanwhile, see CaptureFrame
        if (Rewinding::IsRewinding())
        {
            builtFrame = -1;
            return frameTime;
        }

        // time holds at the start tick until what draws it is back, see AdvanceRestart
        if (holdingStart)
        {
            builtFrame = -1;
            return 0;
        }

        if (!reportsFrameHandOver)
            return !MultiPassEnabled() || GetCurrentPassIndex() == 0 ? frameTime : 0;

        // the frames built since the capture began are numbered from 0, see OnFrameHandedOver
        if (const auto generation = captureGeneration.load(); builtFrameGeneration != generation)
        {
            builtFrameGeneration = generation;
            nextBuiltFrame = 0;
        }
        builtFrame = nextBuiltFrame++;
        if (!MultiPassEnabled())
            return frameTime;

        // time moves on in the frame of the first pass, and the game reads whether to draw the gun while it builds
        const auto passIndex = static_cast<std::size_t>(builtFrame) % captureSettings.passes.size();
        Rendering::SetVisibleElementsOfScene(captureSettings.passes[passIndex].elements);
        return passIndex == 0 ? frameTime : 0;
    }

    void CaptureManager::ToggleCapture()
    {
        if (!isCapturing)
        {
            StartCapture();
        }
        else
        {
            StopCapture();
        }
    }

    // where Iris Studio puts the FFmpeg it downloads, its folder before Iris Studio, else where the codmvm launcher
    // puts it; codmvm's place when none has it
    std::filesystem::path GetFFmpegPath()
    {
        const auto places = Setup::FFmpegPlaces(std::filesystem::path(getenv("LOCALAPPDATA")),
                                                std::filesystem::path(getenv("APPDATA")));
        std::error_code error;
        for (const auto& place : places)
            if (std::filesystem::exists(place, error))
                return place;
        return places.back();
    }

    std::string GetFFmpegCommand(const Components::CaptureSettings& captureSettings, const std::filesystem::path& outputDirectory, const Resolution screenDimensions, std::size_t passIndex)
    {
        auto path = GetFFmpegPath();
        char shortPathBuf[MAX_PATH];
        GetShortPathName(path.string().c_str(), shortPathBuf, MAX_PATH);
        std::string shortPath = shortPathBuf;
        switch (captureSettings.outputFormat)
        {
            case OutputFormat::ImageSequence:
                return std::format(
                    "{} -f rawvideo -pix_fmt bgra -s {}x{} -r {} -i - -q:v 0 "
                    "-vf scale={}:{} -y \"{}\\output_{}_%06d.tga\" 2>&1",
                    shortPath,
                    screenDimensions.width, screenDimensions.height, captureSettings.framerate,
                    captureSettings.resolution.width, captureSettings.resolution.height, outputDirectory.string(), passIndex);
            case OutputFormat::Video:
            {
                std::int32_t profile = 0;
                const char* pixelFormat = nullptr;
                switch (captureSettings.videoCodec.value())
                {
                    case VideoCodec::Prores4444XQ:
                        profile = 5;
                        pixelFormat = "yuv444p10le";
                        break;
                    case VideoCodec::Prores4444:
                        profile = 4;
                        pixelFormat = "yuv444p10le";
                        break;
                    case VideoCodec::Prores422HQ:
                        profile = 3;
                        pixelFormat = "yuv422p10le";
                        break;
                    case VideoCodec::Prores422:
                        profile = 2;
                        pixelFormat = "yuv422p10le";
                        break;
                    case VideoCodec::Prores422LT:
                        profile = 1;
                        pixelFormat = "yuv422p10le";
                        break;
                    default:
                        profile = 4;
                        pixelFormat = "yuv444p10le";
                        LOG_ERROR("Unsupported video codec. Choosing default ({})",
                                  static_cast<std::int32_t>(VideoCodec::Prores4444));
                        break;
                }

                std::string filename = std::format("Pass {}.mov", passIndex);
                auto i = 0;
                while (std::filesystem::exists(outputDirectory / filename))
                {
                    filename = std::format("Pass {}({}).mov", passIndex, ++i);
                }

                return std::format(
                    "{} -f rawvideo -pix_fmt bgra -s {}x{} -r {} -i - -c:v prores -profile:v {} -q:v 1 "
                    "-pix_fmt {} -vf scale={}:{} -y \"{}\\{}\" 2>&1",
                    shortPath, screenDimensions.width, screenDimensions.height, captureSettings.framerate, profile,
                    pixelFormat, captureSettings.resolution.width, captureSettings.resolution.height, outputDirectory.string(), filename);
            }
            default:
                LOG_ERROR("Output format not supported");
                return "";
        }
    }

    void CaptureManager::StartCapture()
    {
        if (captureSettings.startTick >= captureSettings.endTick)
        {
            LOG_ERROR("Start tick must be less than end tick");
            return;
        }

        if (IsRestarting())
            return;

        const bool resizing =
            captureSettings.renderResolution.has_value() && GetBackBufferSize() != captureSettings.renderResolution;

        // the capture starts once the game renders at the size asked for, see OnRenderFrame
        if (resizing)
        {
            switchTarget = captureSettings.renderResolution.value();
            StartRestart(RestartLogic::Kind::Capture);
            switchingRenderSize = true;
            renderSizeChanged = true;
            return;
        }
        expectsReShade = ReShade::IsDrawingEffects();
        BeginCapture();
    }

    void CaptureManager::BeginCapture()
    {
        // ensure output directory exists
        const auto& outputDirectory = PreferencesConfiguration::Get().captureOutputDirectory;
        if (!std::filesystem::exists(outputDirectory))
        {
            std::filesystem::create_directories(outputDirectory);
        }

        // skip to start tick
        auto currentTick = Playback::GetTimelineTick();
        Playback::SetTickDelta(captureSettings.startTick - currentTick, true);

        capturedFrameCount = 0;
        framesWithoutReShade = 0;
        ReShade::TakeDrewEffects();
        watch = {};
        demoEnded = false;
        handedOverFrame = -1;
        drawnFrame = -1;
        captureGeneration++;

        LOG_INFO("Starting capture at {0} ({1} fps)", captureSettings.resolution.ToString(), captureSettings.framerate);

        IDirect3DDevice9* device = D3D9::GetDevice();

        if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        {
            LOG_ERROR("Failed to capture backbuffer");
            StopCapture();
            return;
        }

        D3DSURFACE_DESC bbDesc = {};
        if (FAILED(backBuffer->GetDesc(&bbDesc)))
        {
            LOG_ERROR("Failed to get backbuffer description");
            StopCapture();
            return;
        }
        if (FAILED(device->CreateOffscreenPlainSurface(bbDesc.Width, bbDesc.Height, bbDesc.Format, D3DPOOL_SYSTEMMEM,
                                                       &tempSurface, nullptr)))
        {
            LOG_ERROR("Failed to create temporary surface");
            StopCapture();
            return;
        }

        if (FAILED(device->CreateRenderTarget(bbDesc.Width, bbDesc.Height, bbDesc.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
            &downsampledRenderTarget, NULL)))
        {
            LOG_ERROR("Failed to create render target");
            StopCapture();
            return;
        }

        screenDimensions.width = static_cast<std::int32_t>(bbDesc.Width);
        screenDimensions.height = static_cast<std::int32_t>(bbDesc.Height);

        if (!std::filesystem::exists(GetFFmpegPath()))
        {
            LOG_ERROR("ffmpeg is not present in the game directory");
            ffmpegNotFound = true;
            StopCapture();
            return;
        }
        ffmpegNotFound = false;

        if (captureSettings.passes.empty())
        {
            std::string ffmpegCommand = GetFFmpegCommand(captureSettings, outputDirectory, screenDimensions, 0);
            LOG_DEBUG("ffmpeg command: {}", ffmpegCommand);
            pipe = _popen(ffmpegCommand.c_str(), "wb");
            if (!pipe)
            {
                LOG_ERROR("ffmpeg pipe open error");
                StopCapture();
                return;
            }
        }
        else
        {
            for (std::size_t i = 0; i < captureSettings.passes.size(); i++)
            {
                auto& pass = captureSettings.passes[i];

                std::string ffmpegCommand = GetFFmpegCommand(captureSettings, outputDirectory, screenDimensions, i);
                LOG_DEBUG("ffmpeg command: {}", ffmpegCommand);
                pass.pipe = _popen(ffmpegCommand.c_str(), "wb");
                if (!pass.pipe)
                {
                    LOG_ERROR("ffmpeg pipe open error");
                    StopCapture();
                    return;
                }
            }
        }

        isCapturing.store(true);
    }

    void CaptureManager::StopCapture()
    {
        LOG_INFO("Stopped capture (wrote {0} frames)", capturedFrameCount);
        if (isCapturing && framesWithoutReShade > 0)
        {
            const auto note = RestartLogic::DescribeFramesWithoutReShade(framesWithoutReShade);
            LOG_WARN("{}", note);
            UI::Toasts::Show(note);
        }

        // the game switches back after the capture ended; set before it does, for whoever waits for it on another thread
        holdingStart = false;
        if (renderSizeChanged)
        {
            switchTarget = gameResolution;
            StartRestart(RestartLogic::Kind::Back);
            switchingBack = true;
            switchingRenderSize = false;
        }
        isCapturing.store(false);

        Rendering::ResetVisibleElements();
        framePrepared = false;

        if (pipe)
        {
            fflush(pipe);
            fclose(pipe);
            pipe = nullptr;
        }

        for (auto& pass : captureSettings.passes)
        {
            if (pass.pipe)
            {
                fflush(pass.pipe);
                fclose(pass.pipe);
                pass.pipe = nullptr;
            }
        }

        if (tempSurface)
        {
            tempSurface->Release();
            tempSurface = nullptr;
        }

        if (backBuffer)
        {
            backBuffer->Release();
            backBuffer = nullptr;
        }

        if (downsampledRenderTarget)
        {
            downsampledRenderTarget->Release();
            downsampledRenderTarget = nullptr;
        }

        if (depthSurface)
        {
            depthSurface->Release();
            depthSurface = nullptr;
        }

        if (depthShader)
        {
            depthShader->Release();
            depthShader = nullptr;
        }

        // (asked of the game from AdvanceRestart: ReShade may be loading still, and a reset now would hang)
        renderSizeChanged = false;
    }
}