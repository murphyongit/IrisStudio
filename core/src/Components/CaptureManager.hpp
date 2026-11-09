#pragma once
#include "Camera.hpp"
#include "CaptureWatch.hpp"
#include "RestartLogic.hpp"
#include "Types/RenderingFlags.hpp"

namespace IWXMVM::Components
{
    enum class OutputFormat
    {
        Video,
        CameraData,
        ImageSequence,

        Count
    };

    enum class PassType
    {
        Default,
        Depth,
        Normal,
        Count
    };

    enum class VisibleElements
    {
        Everything,
        WorldAndPlayers,
        OnlyGun,
        OnlyWorld,
        OnlyPlayers,
        Count
    };

    struct PassData
    {
        PassType type;
        VisibleElements elements;
        FILE* pipe = nullptr;
        bool useReshade = true;
    };

    struct Resolution
    {
        int32_t width, height;

        bool operator==(const Resolution& other) const
        {
            return width == other.width && height == other.height;
        }

        std::string ToString() const
        {
            return std::format("{0}x{1}", width, height);
        }
    };

    enum class VideoCodec
    {
        Prores4444XQ,
        Prores4444,
        Prores422HQ,
        Prores422,
        Prores422LT,

        Count
    };

    struct CaptureSettings
    {
        uint32_t startTick, endTick;
        
        OutputFormat outputFormat;
        std::optional<VideoCodec> videoCodec;

        Resolution resolution;
        int32_t framerate;

        std::vector<PassData> passes;

        // the size the game renders at while capturing, when it isn't the window's; see Features_RenderSize
        std::optional<Resolution> renderResolution;
    };

    // the FFmpeg captures are written with (Setup::FFmpegPlaces)
    std::filesystem::path GetFFmpegPath();

    class CaptureManager
    {
       public:
        static CaptureManager& Get()
        {
            static CaptureManager instance;
            return instance;
        }

        CaptureManager(CaptureManager const&) = delete;
        void operator=(CaptureManager const&) = delete;

        void Initialize();
        void ToggleCapture();
        void StartCapture();
        void StopCapture();
        void CaptureFrame();
        void PrepareFrame();

        std::string_view GetOutputFormatLabel(OutputFormat outputFormat);
        std::string_view GetVideoCodecLabel(VideoCodec codec);
        
        CaptureSettings& GetCaptureSettings()
        {
            return captureSettings;
        }

        std::array<Components::Camera::Mode, 3> GetRecordableCameras() 
        {
            return 
            {
                Components::Camera::Mode::FirstPerson, 
                Components::Camera::Mode::Dolly,
                Components::Camera::Mode::Bone
            };
        }

        // the output sizes: the render size and fractions of it, which supersample it; two thirds is the step between
        // (2,560 x 1,440 of 3,840 x 2,160)
        std::array<Resolution, 5> GetSupportedResolutions() const
        {
            const auto base = captureSettings.renderResolution.value_or(gameResolution);
            constexpr std::array<std::pair<std::int32_t, std::int32_t>, 5> fractions = {
                {{1, 1}, {2, 3}, {1, 2}, {1, 3}, {1, 4}}};
            std::array<Resolution, 5> resolutions;
            for (std::size_t i = 0; i < fractions.size(); i++)
            {
                const auto [numerator, denominator] = fractions[i];
                resolutions[i] = {base.width * numerator / denominator, base.height * numerator / denominator};
            }
            return resolutions;
        }

        // the sizes the game can render at for a capture, larger than its window
        std::vector<Resolution> GetRenderResolutions() const;

        Resolution GetGameResolution() const
        {
            return gameResolution;
        }

        // The restarts of the game's renderer the editor waits for, with the game view's loading screen
        // (RestartLogic): a capture's render size and the switch back after it, and
        // the Look tab's Shadow quality. Each lasts until ReShade has loaded its effects again and Pack and Surface depth
        // are back, when they're on. IsSwitchingRenderSize: a capture's, also while switching back after it
        bool IsSwitchingRenderSize() const
        {
            return switchingRenderSize || switchingBack;
        }
        bool IsRestarting() const
        {
            return IsSwitchingRenderSize() || switchingShadows;
        }
        // what the loading screen says, and what is under it for what is waited for now
        std::string GetRestartLabel() const;
        RestartLogic::Screen GetRestartScreen() const;
        // (the game view's) the loading screen was drawn this frame
        void OnRestartScreenDrawn();
        // the Look tab's Shadow quality: the sun's shadow map at size, through a restart (name: "High")
        void RestartForShadows(std::int32_t size, std::string_view name);

        // the restarts that ended, the last one last (up to 8), for the log and the harness
        struct RestartRecord
        {
            RestartLogic::Kind kind;
            std::string label;
            RestartLogic::Step end;  // Done, or a timeout
            std::uint32_t heldMs = 0;  // held back while ReShade loaded
            std::uint32_t gameMs = 0;
            std::uint32_t reshadeMs = 0;
            std::uint32_t packMs = 0;
            std::uint32_t surfaceMs = 0;
            bool waitedForReShade = false;
            bool waitedForPack = false;
            bool waitedForSurface = false;
            bool reshadeTimedOut = false;
            bool packTimedOut = false;
            bool surfaceTimedOut = false;
            std::uint32_t screenFrames = 0;
        };
        std::vector<RestartRecord> GetRestarts() const;
        // (the harness's) how long ReShade is waited for, and a restart held back for it (none: RestartLogic's)
        void SetRestartTimeouts(std::optional<std::uint64_t> reshadeMs, std::optional<std::uint64_t> holdMs);

        // what the last capture's first frame had, for the harness: Pack's textures swapped in and those still drawn
        // without theirs, and the surfaces that drew with Surface depth's shaders and what it had left
        struct FirstFrame
        {
            std::uint32_t packSwaps = 0;
            std::uint32_t packLeft = 0;
            std::uint32_t surfaceDraws = 0;
            std::uint32_t surfaceLeft = 0;
        };
        FirstFrame GetFirstFrame() const
        {
            return firstFrame;
        }

        // the frames of the last capture drawn without ReShade's effects while it was expected (ReShade loading)
        std::uint32_t GetFramesWithoutReShade() const
        {
            return framesWithoutReShade;
        }

        std::array<int32_t, 6> GetSupportedFramerates()
        {
            return { 50, 100, 125, 250, 500, 1000 };
        }

        bool IsCapturing() const
        {
            return isCapturing;
        }

        bool IsFramePrepared() const
        {
            return framePrepared;
        }

        bool IsFFmpegPresent() const
        {
            return !ffmpegNotFound;
        }

        std::int32_t GetCapturedFrameCount() const
		{
			return capturedFrameCount;
		}

        bool MultiPassEnabled() const
        {
            return !captureSettings.passes.empty();
        }

        const PassData& GetCurrentPass() const
        {
            return captureSettings.passes[GetCurrentPassIndex()];
        }

        int32_t OnGameFrame();

        // For games that draw on a render thread of their own, a frame or two behind the game: called on the game's
        // thread when the render thread waits for the frame the game built, and on the render thread once it drew
        // it (in EndScene). The frames built while capturing are numbered on the game's thread, the number follows
        // the frame to the render thread, and the pass of a frame follows from it: whether time moves on and the gun
        // is drawn as the game builds it, what the render thread skips as it draws it, and which file it goes to.
        // Reading the number of captured frames on the game's thread instead raced the render thread, so passes
        // came a frame apart for stretches of a capture.
        void OnFrameHandedOver();
        void OnFrameDrawn();
        // The timeline's tick of the frame drawn last, which the frame's ReShade values are keyed at:
        // the frame's tick as the game built it, followed to the render thread as its number is; the timeline's
        // tick for games without a render thread of their own
        std::uint32_t GetDrawnTick() const;

        // On the game's thread as the demo reads its end (and rewinds a second): a capture stops there, see
        // CaptureWatch
        void OnDemoEnd();

       private:
        CaptureManager()
        {
        }

        // StartCapture once the game renders at the capture's size
        void BeginCapture();

        void OnRenderFrame();
        // a frame of the restart waited for, and its end
        void AdvanceRestart();
        void StartRestart(RestartLogic::Kind kind);
        // asks the game for it (RestartLogic::Step::Ask); false if it can't
        bool AskForRestart(RestartLogic::Kind kind);
        void CancelRestart();
        void RecordRestart(const RestartLogic::Wait& wait, RestartLogic::Step end);

        // the pass of the frame captured next
        std::size_t GetCurrentPassIndex() const;

        // a capture that can't reach its end tick stops, saying why (CaptureWatch)
        void StopEarly(CaptureWatch::Stop stop);

        bool initialized = false;
        Resolution gameResolution = Resolution(0, 0);
        CaptureSettings captureSettings;

        // a capture that renders at another size waits for the game to switch to it, and switches back when it ends;
        // Shadow quality waits likewise (see IsRestarting)
        std::atomic_bool switchingRenderSize = false;
        std::atomic_bool switchingBack = false;
        std::atomic_bool switchingShadows = false;
        // a capture's frames begin at the start tick, and time holds there until what draws them is back (RestartLogic)
        std::atomic_bool holdingStart = false;
        Resolution switchTarget = Resolution(0, 0);
        bool renderSizeChanged = false;
        std::int32_t shadowSize = 0;
        std::string shadowName;
        std::optional<RestartLogic::Wait> restart;  // the render thread's
        std::optional<std::uint64_t> reshadeTimeout;
        std::optional<std::uint64_t> holdTimeout;
        mutable std::mutex restartsMutex;
        std::vector<RestartRecord> restarts;
        RestartLogic::Screen restartScreen;
        // ReShade's effects expected in the capture's frames (it drew them as the capture began, or was waited for),
        // and the frames captured without them
        bool expectsReShade = false;
        std::atomic<std::uint32_t> framesWithoutReShade = 0;
        FirstFrame firstFrame;

        IDirect3DSurface9* depthSurface = nullptr;
        IDirect3DPixelShader9* depthShader = nullptr;

        // internal capture state
        Resolution screenDimensions = Resolution(0, 0);
        IDirect3DSurface9* backBuffer = nullptr;
        IDirect3DSurface9* downsampledRenderTarget = nullptr;
        IDirect3DSurface9* tempSurface = nullptr;
        std::atomic_bool isCapturing = false;
        std::int32_t capturedFrameCount = 0;
        CaptureWatch::Watch watch;
        std::atomic_bool demoEnded = false;

        // see OnFrameHandedOver; -1 for frames that aren't captured
        bool reportsFrameHandOver = false;
        std::atomic<std::int32_t> captureGeneration = 0;
        std::int32_t builtFrameGeneration = 0;  // the game's thread
        std::int32_t nextBuiltFrame = 0;        // the game's thread
        std::int32_t builtFrame = -1;           // the game's thread
        std::atomic<std::int32_t> handedOverFrame = -1;
        std::int32_t drawnFrame = -1;           // the render thread
        std::atomic<std::uint32_t> handedOverTick = 0;
        std::atomic<std::uint32_t> drawnTick = 0;
        bool ffmpegNotFound = false;
        bool framePrepared = false;
        FILE* pipe = nullptr;
    };
}  // namespace IWXMVM::Components