// What the editor's files share (ui.cpp draws the editor; look.cpp the Look tab; capture.cpp the Record tab).
#pragma once
#include "theater.hpp"

#include <d3d11.h>
#include <imgui.h>

// Font Awesome 6 (solid), the icons the editor uses
#define ICON_FILM "\xef\x80\x88"
#define ICON_VIDEO "\xef\x80\xbd"
#define ICON_SLIDERS "\xef\x87\x9e"
#define ICON_PERSON "\xef\x86\x83"
#define ICON_CIRCLE "\xef\x84\x91"
#define ICON_CAMERA "\xef\x80\xb0"
#define ICON_LOCK "\xef\x80\xa3"
#define ICON_LOCK_OPEN "\xef\x8f\x81"
#define ICON_BACKWARD_STEP "\xef\x81\x88"
#define ICON_FORWARD_STEP "\xef\x81\x91"
#define ICON_PLAY "\xef\x81\x8b"
#define ICON_PAUSE "\xef\x81\x8c"
#define ICON_STOP "\xef\x81\x8d"
#define ICON_FLAG "\xef\x80\xa4"
#define ICON_TRASH "\xef\x87\xb8"
#define ICON_SUN "\xef\x86\x85"
#define ICON_CLOUD "\xef\x83\x82"
#define ICON_LIGHTBULB "\xef\x83\xab"
#define ICON_EYE "\xef\x81\xae"
#define ICON_BURGER "\xef\xa0\x85"
#define ICON_WAND "\xef\x83\x90"
#define ICON_SEARCH "\xef\x80\x82"
#define ICON_FOLDER_OPEN "\xef\x81\xbc"
#define ICON_ROTATE "\xef\x8b\xb1"
#define ICON_PLUS "\x2b"

// ui.cpp
// A section's header (IrisStudio's): true while it is open. With reset, a "Reset" button at its right.
bool ui_section(const char* icon, const char* label, bool* reset = nullptr, bool open_by_default = true);
// A row: the label on the left, the next control from 40% across.
void ui_row_label(const char* label);
float ui_font_size();
ImFont* ui_bold_font();
void ui_not_built(const char* what);
void ui_log(const char* format, ...);
// A text box of the editor has the keyboard.
bool ui_typing();

// look.cpp: the Look tab, and putting the game's settings back when playback ends
void look_draw_tab(const TheaterState& state);
void look_frame(const TheaterState& state);
void look_toggle_hud();

// capture.cpp: the Record tab; a frame of the game's picture as it was drawn (before the editor)
void capture_draw_tab(const TheaterState& state);
void capture_frame(const TheaterState& state, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* picture,
                   DXGI_FORMAT format);
bool capture_wants_picture();
