#pragma once
#include <windows.h>
#include <cstdint>

// Starts the mod. Safe to call more than once; only the first call does anything.
void theater_start(HMODULE self);

// What the panel (ui.cpp) shows and asks of playback. The asks are taken up at the next frame's start.
struct TheaterState
{
	bool playing = false, starting = false, paused = false;
	bool settled = false;        // every message due has been given: the picture is where the clock is
	float capture_fps = 0.0f;    // recording to video at this rate (0: not)
	float speed = 1.0f;
	std::uint32_t position_ms = 0, length_ms = 0;
	double position_exact = 0.0;  // the position with the part of a millisecond playback keeps
	char name[96] = {}, map[64] = {};
};

TheaterState theater_state();
void theater_pause_toggle();
void theater_slower();
void theater_faster();
void theater_seek(std::uint32_t ms);
// The speed nearest to one of the steps (IrisStudio's timescale steps); the steps themselves.
void theater_set_speed(float speed);
int theater_speed_steps(const float** steps);
void theater_stop();
void theater_set_paused(bool paused);
// Recording: the clock goes on by one frame of this rate for every frame drawn (0 ends it).
void theater_capture(float fps);
// The game's settings ("dvars"): one by name, how many there are, one by its place in the sorted list.
// A dvar's fields are read where they lie (theater.cpp has the layout).
void* theater_dvar(const char* name);
int theater_dvar_count();
void* theater_dvar_at(int index);
// The game's folder.
const char* theater_folder();
const char* theater_version();

// The recordings in the theater folder that can be played, newest first.
struct RecordingInfo
{
	char file[64] = {};
	char map[64] = {}, gametype[64] = {};
	std::uint64_t size = 0;
	std::uint32_t length_ms = 0;
};

int theater_recordings(RecordingInfo* out, int max);
// Plays one (by its file name); what is playing is stopped first.
void theater_play(const char* file);
// Nothing is playing or about to, and no match is being recorded: the game is at its menus.
bool theater_idle();
// A console command for the game, run at the next frame's start.
void theater_command(const char* text);
// The game's console is open: keys are being typed, not meant for the editor.
bool theater_keys_blocked();
HMODULE theater_module();
void theater_log(const char* text);

// Hooks the game's drawing so the panel is drawn over it. MinHook must be initialised.
void ui_start();

// The panel's side of the mouse (ui.cpp). The game puts the pointer back in the middle every frame; while
// the panel or the free camera has the mouse it is kept from doing so.
HWND ui_window();
// A text box of the editor has the keyboard.
bool ui_typing();
bool ui_mouse_is_panels();
void ui_camera_has_mouse(bool on);
// How far the pointer moved since the last call (it is put back in the middle of the game's window).
void ui_take_mouse_motion(int* dx, int* dy);
// The mouse wheel's notches since the last call (up is positive).
int ui_take_mouse_wheel();

// The cameras (camera.cpp).
enum class CameraMode { FirstPerson, ThirdPerson, Free, Orbit, Dolly, Bone, Count };

struct CameraState
{
	CameraMode mode = CameraMode::FirstPerson;
	float fov = 80.0f, speed = 400.0f;
	int points = 0;
	bool body = false;
	std::uint32_t point_ms[64] = {};
};

CameraState camera_state();
void camera_set_mode(CameraMode mode);
void camera_set_speed(float speed);
void camera_set_fov(float fov);
void camera_add_point();
void camera_clear_points();
// The player's own body drawn (the game's third person), for the cameras away from the eyes.
void camera_set_body(bool on);
// Playback is over: the game's own view settings again.
void camera_playback_ended();
void camera_start();

// The sun as a light (sun.cpp): its colour, brightness and direction, through the engine's own overrides.
struct SunState
{
	bool known = false;                  // the map has a sun and it has been seen
	float color[3] = {1.0f, 1.0f, 1.0f};
	float brightness = 1.0f;
	float pitch = 0.0f, yaw = 0.0f;
	bool changed = false;
	bool color_set = false, brightness_set = false, direction_set = false;
};

SunState sun_state();
void sun_set_color(const float color[3]);
void sun_set_brightness(float brightness);
void sun_set_direction(float pitch, float yaw);
void sun_reset();
void sun_start();
