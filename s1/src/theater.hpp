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
};

TheaterState theater_state();
void theater_pause_toggle();
void theater_slower();
void theater_faster();
void theater_seek(std::uint32_t ms);
void theater_stop();
void theater_log(const char* text);

// Hooks the game's drawing so the panel is drawn over it. MinHook must be initialised.
void ui_start();
