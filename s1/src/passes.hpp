// S1iris: passes (what of the picture is drawn, and its depth), the gun's depth, the fog's own values.
#pragma once
#include "theater.hpp"

#include <d3d11.h>
#include <dxgi.h>

// Hooks the game's drawing steps a pass holds back. MinHook must be initialised.
void passes_start();
// Each frame on the game's own thread, before the scene is put together.
void passes_game_frame(const TheaterState& state);
// As a frame is shown, before the editor copies the picture: the depth is drawn over it when asked for.
void passes_before_copy(IDXGISwapChain* swap_chain, ID3D11Device* device, ID3D11DeviceContext* context, bool playing);
// The picture's size is changing: nothing of the old one is held on to.
void passes_release();
// The Record tab's section (busy: a recording is under way).
void passes_draw_ui(bool busy);
// The Look tab's Fog section: the fog's own colour and numbers.
void passes_draw_fog();
// The passes a recording can be made of: how many, which are ticked, their names; one put on (-1: the
// whole picture again), and the word for the pass now on in a file's name ("" for none).
int passes_count();
bool passes_ticked(int index);
const char* passes_label(int index);
void passes_use(int index);
const char* passes_tag();
