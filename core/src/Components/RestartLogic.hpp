#pragma once

namespace IWXMVM::Components::RestartLogic
{
    // The editor's wait for a restart of the game's renderer, apart from the game so
    // the tests in tests/actors-tests build it on their own: a capture's render size, the switch back after it, and the
    // Look tab's Shadow quality. The game resets its device, about 5 s with no frames, and that takes what was on the
    // card with it:
    // - ReShade loads its effects again, whether its effects toggle is on or off, drawing the game without them and its
    //   loading ring in the corner: about 2 s from its cache, and minutes for a size it hasn't compiled
    //   them at before (they take the frame's size as a constant). A reset during that waits inside ReShade for the
    //   compile, with the game hung, so a restart asked for while ReShade loads is held back until it's done.
    // - Pack's textures and Surface depth's heights are made again: what the frame draws, and for a capture the
    //   rest of the pack too, as the camera will move. A capture goes to its start tick first and holds there, so what
    //   is waited for is what its first frame draws.
    // The wait, and the loading screen with it, lasts until all of them are done or out of time. The screen says what
    // the wait is on, and the game is asked once it has been drawn, so the picture of the window kept through the reset
    // has it.

    // the game has this long to make the change, as before; ReShade longer, for its compiles at a new size (over two
    // minutes on the test PC); a restart is held back as long again
    inline constexpr std::uint64_t GAME_TIMEOUT_MS = 30000;
    inline constexpr std::uint64_t RESHADE_TIMEOUT_MS = 300000;
    inline constexpr std::uint64_t HOLD_TIMEOUT_MS = 300000;
    // ReShade is back once it drew its effects in this many frames after the reset (a technique it hasn't made yet is
    // left out of a frame and made for the next), or has loaded them when its toggle has them off
    inline constexpr std::uint32_t RESHADE_READY_FRAMES = 3;
    // loaded, with its toggle on, and drawing nothing this long after: there's nothing for it to draw
    inline constexpr std::uint64_t RESHADE_DRAW_MS = 3000;
    // Pack and Surface depth: nothing left to make for this many frames in a row, and this long at most, for a capture's
    // first frames and for a view in the editor, which the workers go on filling behind the wait
    inline constexpr std::uint32_t LOAD_READY_FRAMES = 10;
    inline constexpr std::uint64_t CAPTURE_LOAD_TIMEOUT_MS = 60000;
    inline constexpr std::uint64_t VIEW_LOAD_TIMEOUT_MS = 10000;
    // the frames of the loading screen before the game is asked: the window has it by then
    inline constexpr std::uint32_t SCREEN_FRAMES = 4;
    // the screen shows the time waited after this long, and why ReShade is taking so long after this long on it
    inline constexpr std::uint64_t SHOW_TIME_MS = 10000;
    inline constexpr std::uint64_t SHOW_NOTE_MS = 20000;

    enum class Kind
    {
        Capture,  // a capture's render size, the capture starting after it
        Back,     // the window's size again after a capture
        Shadows,  // the Look tab's Shadow quality
    };

    enum class Step
    {
        Held,     // not asked of the game yet: the screen's first frames, and while ReShade loads its effects
        Ask,      // (once) ask the game for the restart now
        Game,     // the game hasn't made the change yet
        ReShade,  // ReShade loads its effects
        AtStart,  // (once, a capture) go to the start tick and hold there
        Pack,     // Pack's textures are made again
        Surface,  // Surface depth's heights and shaders are made again
        Done,
        GameTimedOut,  // the game didn't make it in GAME_TIMEOUT_MS
    };

    // ReShade now, as ReShade::GetReload tells it
    struct ReShadeState
    {
        bool present = false;           // in the game: it loads its effects again after a reset
        bool effectsOn = true;          // its toggle
        bool loading = false;           // loading them now, from a reset until they're all made
        std::uint32_t framesSince = 0;  // the frames it drew them in since the reset
    };

    // Pack or Surface depth now: on with the Look tab, and what is left to make
    struct Load
    {
        bool on = false;
        std::uint32_t drawn = 0;  // of what the last frames drew (a surface without its depth)
        std::uint32_t ahead = 0;  // of what they will draw (Pack's read-ahead), waited for by a capture only
    };

    // A frame of the wait
    struct Frame
    {
        std::uint32_t resets = 0;  // the device's resets so far
        bool changed = false;      // the game has the change: its render size or its shadow map's
        bool moving = false;       // a rewind to the capture's start tick: the frames are of another moment
        ReShadeState reshade;
        Load pack;
        Load surface;
    };

    // The wait for Pack or for Surface depth
    struct LoadWait
    {
        bool on = false;  // it was on, so it was waited for
        bool timedOut = false;
        std::optional<std::uint64_t> started;  // ms
        std::optional<std::uint64_t> done;     // ms
        std::uint32_t clearFrames = 0;
        std::uint32_t left = 0;  // what is left now, and the most that was
        std::uint32_t most = 0;
    };

    struct Wait
    {
        Kind kind = Kind::Capture;
        std::uint64_t created = 0;                 // ms: since the screen came up
        std::uint64_t started = 0;                 // ms: since it was asked of the game
        std::uint32_t resets = 0;                  // the device's resets when it was
        bool held = true;                          // not asked of the game yet
        std::uint32_t heldFrames = 0;
        bool heldForReShade = false;               // ReShade was loading meanwhile
        std::uint32_t heldMs = 0;                  // how long for ReShade
        std::uint64_t reshadeTimeout = RESHADE_TIMEOUT_MS;
        std::uint64_t holdTimeout = HOLD_TIMEOUT_MS;
        std::optional<std::uint64_t> gameDone;     // ms, when the game had made the change
        bool waitsForReShade = false;              // ReShade is in the game
        std::optional<std::uint64_t> loadedAt;     // ms, when it had loaded its effects
        std::optional<std::uint64_t> reshadeDone;  // ms
        bool reshadeTimedOut = false;
        bool atStart = false;                      // a capture is at its start tick
        LoadWait pack;
        LoadWait surface;
        std::uint32_t screenFrames = 0;            // the frames the loading screen was drawn in
    };

    Wait Start(Kind kind, std::uint64_t now, std::uint32_t resets);

    // A frame of the wait, now in ms. The game's change counts once the device was reset after it was asked for.
    Step Advance(Wait& wait, std::uint64_t now, const Frame& frame);

    // what the loading screen says: "SWITCHING TO 3840x2160 FOR THE CAPTURE", "SWITCHING BACK TO 1920x1080",
    // "SWITCHING SHADOW QUALITY TO HIGH"; what is a size ("3840x2160") or a Shadow quality's name ("High")
    std::string GetLabel(Kind kind, std::string_view what);
    // What it says under that: the step, with its count, the time waited once it's a while, and why a long wait is long
    struct Screen
    {
        std::string line;       // "Loading Pack 420 of 1,495"
        std::string time;       // "2:14"
        std::string_view note;  // "First time at this size, a few minutes"
    };
    Screen GetScreen(const Wait& wait, Step step, std::uint64_t now);

    // The screen's ring: the share of a turn it's at by the clock, so the editor's frames and what paints it between
    // them agree, and how much of the turn its arc takes
    inline constexpr float RING_ARC = 0.28f;
    float GetRingTurn(std::uint64_t now);
    // how much the ring's track and its arc cover a pixel dx, dy from its middle, 0 to 1
    struct RingPixel
    {
        float track = 0.0f;
        float arc = 0.0f;
    };
    RingPixel GetRingPixel(float dx, float dy, float radius, float thickness, float turn);

    // The toast after a capture with frames drawn without ReShade's effects (ReShade took longer than
    // RESHADE_TIMEOUT_MS, or loaded an effect again meanwhile): "ReShade was loading for 12 frames of the capture"
    std::string DescribeFramesWithoutReShade(std::uint32_t frames);
    // The toast when Pack or Surface depth ran out of time before a capture's first frame
    std::string DescribeNotReady(std::string_view what);
}  // namespace IWXMVM::Components::RestartLogic
