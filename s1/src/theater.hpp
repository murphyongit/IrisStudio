#pragma once
#include <windows.h>
#include <cstdint>

// Starts the mod. Safe to call more than once; only the first call does anything.
void theater_start(HMODULE self);

// What the panel (ui.cpp) shows and asks of playback. The asks are taken up at the next frame's start.
struct TheaterState
{
	bool playing = false, starting = false, paused = false;
	float speed = 1.0f;
	std::uint32_t position_ms = 0, length_ms = 0;
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
const char* theater_version();
HMODULE theater_module();
void theater_log(const char* text);

// Hooks the game's drawing so the panel is drawn over it. MinHook must be initialised.
void ui_start();

// The panel's side of the mouse (ui.cpp). The game puts the pointer back in the middle every frame; while
// the panel or the free camera has the mouse it is kept from doing so.
HWND ui_window();
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
	std::uint32_t point_ms[64] = {};
};

CameraState camera_state();
void camera_set_mode(CameraMode mode);
void camera_set_speed(float speed);
void camera_set_fov(float fov);
void camera_add_point();
void camera_clear_points();
void camera_start();
