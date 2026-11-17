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
	std::uint32_t rewinds = 0;    // how many times playback has gone back (the game begins its match again each time)
	double position_exact = 0.0;  // the position with the part of a millisecond playback keeps
	char name[96] = {}, map[64] = {};
	// (0.78) where the recording really is while a jump is still playing up to its time (position_ms is then
	// the time being gone to); whether every message due has been given; a search for kills is running
	double clock_ms = 0.0;
	bool caught_up = false, scanning = false;
	// (0.79) a jump is under way: its messages are being given, the HUD emptied, or its last seconds played up to it
	bool jumping = false;
};

TheaterState theater_state();
// The whole recording played through fast from its start (for the list of kills), then back to where it was.
void theater_scan();
void theater_pause_toggle();
void theater_slower();
void theater_faster();
void theater_seek(std::uint32_t ms);
// The recording given again from its start up to where it is: the game makes every model anew.
void theater_rebuild();
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
// The HUD's script is to act on something it was just told: in playback the HUD's clock is the
// recording's (it stands still while paused), and for half a second it is given real time.
void theater_hud_wake();
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
	bool clean = true;        // outside first person: none of the player's own HUD or hit effects
	bool player_aim = false;  // the game's own workings keep the player's view (scorestreaks aim from the player)
	std::uint32_t point_ms[64] = {};
};

// The bone camera: on a part of an entity's model (a player's head, hand, gun), moved along that part's
// own axis (forward, right, up) and turned (pitch, yaw, roll) with it, or not turning with it.
struct BoneCamera
{
	int entity = -1;  // -1: the one nearest the camera when the bone camera is first used
	char bone[48] = "j_head";
	float offset[3] = {}, turn[3] = {};
	bool rotate = true;
};

struct BoneState
{
	BoneCamera camera;
	std::uint64_t entities = 0;  // those with a model that has a head
	int nearest = -1;
	bool found = false;
};

// A light added to the scene: a point that shines all round.
struct SceneLight
{
	bool on = true;
	float origin[3] = {};
	float color[3] = {1.0f, 1.0f, 1.0f};
	float intensity = 256.0f, radius = 400.0f;  // (the game's own lights' colours are in the hundreds)
	// a spot light: a cone that shines one way (pitch, yaw, roll), and can cast shadows; the scene takes two
	bool spot = false, shadows = true;
	float angles[3] = {}, cone = 60.0f;
};

// models.cpp: what an entity is made of, and models drawn as others (to: "" leaves the model out, null
// ends the swap).
int models_of(int entity, char names[][64], int max);
void models_swap(int entity, const char* from, const char* to);
bool models_swapped(int entity, const char* from, char to[64]);
int models_camos(const char* model, char materials[][64], int max);
void models_set_camo(int entity, const char* model, const char* material);
bool models_camo(int entity, const char* model, char material[64]);
bool models_camo_title(const char* material, char out[64]);
void models_add(int entity, const char* name, bool on);
int models_added(int entity, char names[][64], int max);
void models_clear(int entity);
bool models_pending(int entity);
int models_slot(const char* name);
int models_find(const char* text, int group, int slot, char names[][64], int max);
int models_sets(char names[][32], int max);
int models_apply_set(int entity, const char* name);
int models_randomize(std::uint64_t entities);
int models_randomize_one(int entity);
// An outfit, as the list the game makes a model from: each part's model and materials by name, and the rest of
// its entry as the game had it.
struct ModelsOutfit
{
	int count = 0;
	char names[32][64] = {};
	char over[32][64] = {}, base[32][64] = {};
	unsigned char entries[32][0x20] = {};
};
bool models_outfit_of(int entity, ModelsOutfit* out);
void* models_make_actor(int handle, const ModelsOutfit& outfit, void* tree, char missing[64]);
void models_drop(int entity);
// An outfit of one model (a prop).
void models_outfit_named(const char* model, ModelsOutfit* out);
// The editor's changes for an entity (other models, added ones, camouflage) as text, and put back from it.
int models_export(int entity, char* out, int size);
void models_import(int entity, const char* text);

// ---- actors (actors.cpp): figures the editor puts into the match, dressed as a player and played by clips
constexpr int ACTORS_MAX = 16, ACTOR_CLIPS_MAX = 24, ACTOR_BONES = 22, ACTOR_PATH_MAX = 16, ACTOR_EVENTS_MAX = 12;
// Something an actor does at a time: 0 shots (its gun's flash again and again for a while), 1 an effect once on
// a part of it (blood).
struct ActorEvent
{
	int kind = 0;
	std::uint32_t at_ms = 0, length_ms = 600;
	float every_ms = 100.0f;         // shots: one this often
	char effect[96] = "";
	char bone[32] = "";
};
struct ActorClip
{
	char anim[64] = "";
	std::uint32_t at_ms = 0;         // when it begins, and for how long it runs (then it holds its last pose)
	std::uint32_t length_ms = 2000;
	float speed = 1.0f;
	std::uint32_t blend_ms = 250;    // how long it takes to blend in over the clip before it
	int loop = -1;                   // goes round and round: -1 as the animation itself says, 0 no, 1 yes
};
struct Actor
{
	int handle = 0;                  // the entity its model is made under (the editor's Models list goes by it)
	char name[32] = "";
	bool on = true;
	float origin[3] = {};
	float yaw = 0.0f;
	bool moves = true;               // carried along by its animations (a walk walks)
	bool stays = true;               // holds its last pose after its last clip (else it goes)
	int clip_count = 0;
	ActorClip clips[ACTOR_CLIPS_MAX];
	// (what follows was added after 0.70: a saved scene from before has none of it. Only ever add at the end.)
	// a path: from where it stands through these points. An actor's moving animations carry it along the path
	// (turning at its corners) instead of straight on; a prop goes along it at its speed.
	int path_count = 0;
	float path[ACTOR_PATH_MAX][3] = {};
	// a prop: one model of the game's (no animations), shown for its first clip's time
	bool prop = false;
	char model[64] = "";
	float pitch = 0.0f, roll = 0.0f;
	float speed = 300.0f;            // along its path, units a second
	int event_count = 0;
	ActorEvent events[ACTOR_EVENTS_MAX];
	// (added after 0.77) at each of its shots: a light for a moment (its colour, how far it reaches), and a
	// shell thrown out of its gun (an effect; "": none)
	bool shot_light = true;
	float shot_light_color[3] = {1.0f, 0.72f, 0.42f};
	float shot_light_reach = 420.0f;
	char shell_effect[96] = "";
};
struct ActorNow
{
	bool made = false, shown = false, broken = false;
	int clip = -1;                   // the clip playing
	float origin[3] = {};            // where it stands now (its animations may have carried it)
	float yaw = 0.0f;
	char waiting[96] = "";           // what it is waiting for (a model or animation not in memory)
	float path_gone = 0.0f, path_length = 0.0f;  // how far along its path it is, of how far
	std::uint32_t path_end_ms = 0;   // when it gets to its path's end (0: it does not, as its animations are)
};
int actors_list(Actor* out, int max);
bool actors_now(int index, ActorNow* out);
int actors_handles(int* out, int max);
int actors_add(const float origin[3], float yaw, int dressed_as, std::uint32_t ms);
// A prop: one model of the game's at a place (-1: no room).
int actors_add_prop(const char* model, const float origin[3], float yaw, std::uint32_t ms);
// Another like one (its outfit and the editor's changes to it too), a step to its side (-1: no room).
int actors_duplicate(int index);
// Straight down from a place: what is under it (false: nothing within reach).
bool camera_ground_under(const float at[3], float out[3]);
// (the game's thread) A light for a moment (a shot's flash): from a time of the recording, for so long, fading.
void scene_flash_light(const float origin[3], const float color[3], float radius, float intensity, double from_ms, double length_ms);
// The camera held by hand: how much it sways (0: not at all) and how fast.
void camera_shake(float* amount, float* speed);
void camera_set_shake(float amount, float speed);
// The gun in an actor's hands: which model it is ("" none), and another put there (false: no player's gun to go by).
bool actors_gun(int index, char out[64]);
bool actors_give_gun(int index, const char* model);
// Everything about the actors, for the saved scene and undo: taken, and put back.
int actors_export(Actor* actors, ModelsOutfit* outfits, int max);
void actors_import(const Actor* actors, const ModelsOutfit* outfits, int count);
// The actor (by its handle) is made again: its outfit's changes are taken up.
void actors_touch(int handle);
bool actors_origin_of(int handle, float out[3]);
int deaths_export(char* out, int size);
void deaths_import(const char* text);
void actors_set(int index, const Actor& actor);
bool actors_dress(int index, int dressed_as);
void actors_remove(int index);
int actors_animations(const char* text, int group, char names[][64], int max);
float actors_animation_length(const char* name, bool* loops);
void actors_preview(const char* animation, int dressed_as_actor, int dressed_as_entity);
bool actors_entity_origin(int handle, float out[3]);
// death animations for the match's own players (-1: everybody without a choice of their own)
bool deaths_any();
void deaths_set(int entity, const char* animation);
bool deaths_get(int entity, char out[64]);
void deaths_on_create(void* tree, int whose, int handle);
bool actors_preview_bones(float out[ACTOR_BONES][3], bool has[ACTOR_BONES], float* fraction);
void actors_release();
void actors_scene_frame(const TheaterState& state);
void models_status(char out[200]);
void models_allow_loading(bool on);
bool models_loading_allowed();
void models_game_frame(const TheaterState& state);
void models_frame(const TheaterState& state);
void models_start();
void effects_start();
// effects.cpp: effects of the game's played in the scene.
constexpr int EFFECTS_MAX = 128;

struct PlacedEffect
{
	bool on = true;
	char name[96] = "";
	float origin[3] = {};
	float angles[3] = {-90.0f, 0.0f, 0.0f};  // (an effect goes along its forward: up, to begin with)
	std::uint32_t at_ms = 0, every_ms = 0;  // when in the recording, and again every so often (0: once)
	std::uint32_t length_ms = 2000;         // its clip on the timeline: played again and again only this long
	bool follow = false;                    // played where the camera is each time (rain, mist), this far above it
	float above = 0.0f;
	// (added after 0.70; only ever add at the end) played where an actor or prop is each time: its handle (0: not)
	int on_actor = 0;
};

int effects_find(const char* text, char names[][96], int max);
int effects_extra_state();
void effects_load_extra();
int effects_placed(PlacedEffect* out, int max);
int effects_live(bool* out, int max);
int effects_lasts(const char* name);
int effects_add(const char* name, const float origin[3], std::uint32_t ms);
void effects_set(int index, const PlacedEffect& placed);
void effects_fire(int index);
void effects_remove(int index);
// (the game's thread) An effect played once, now, at a place along an axis (forward, left, up). False: not loaded.
bool effects_play_once(const char* name, const float origin[3], const float axis[3][3]);
// The camera's path as bytes (for the saved scene and undo), and put back.
int camera_points_export(unsigned char* out, int size);
void camera_points_import(const unsigned char* bytes, int size);
// scene.cpp: the saved scene (what the editor put into a recording, kept beside it), undo and redo, the storm.
struct StormState
{
	bool on = false;
	float every_s = 8.0f;      // a strike about this often
	float flash = 3.0f;        // how much brighter the sun's light is at a strike
	float distance = 6000.0f;  // how far from the camera the bolts are
	bool bolts = true;
	// (added after 0.77; kept here because this is saved with the recording) how strongly the surfaces' normal
	// maps stand out: 1 the game's own (surfaces.cpp)
	float bump = 1.0f;
};
StormState storm_state();
void storm_set(const StormState& storm);
void scene_ui_frame(const TheaterState& state, bool busy, bool touched);
void scene_game_frame(const TheaterState& state);
// The other recordings that have a saved scene with effects in it (their names, and how many effects each has);
// and those effects (with their places and times) added to this recording's.
int scene_recordings(char names[][96], int counts[], int max);
int scene_take_effects(const char* recording);
// hudkeep.cpp: the HUD kept out of ReShade's effects.
struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;
void hud_keep_frame(IDXGISwapChain* real, ID3D11Device* device, ID3D11DeviceContext* context);
void hud_keep_release();
void picture_hold(IDXGISwapChain* real, ID3D11Device* device, ID3D11DeviceContext* context, bool playing, bool hold);
// surfaces.cpp: bump strength.
void surfaces_frame(ID3D11Device* device, ID3D11DeviceContext* context, float strength);
void surfaces_status(int* found, int* made, int* megabytes);
bool hud_keep_get();
void hud_keep_set(bool on);
const char* hud_keep_status();
bool look_save_file(const char* path);
bool look_load_file(const char* path);
// The kills seen in the recording so far (scene.cpp hooks the game's "a kill for the kill feed"): when, who (the
// players' numbers), and the picture shown for it (the weapon's).
struct SceneKill
{
	std::uint32_t ms = 0;
	int attacker = -1, victim = -1;
	char icon[64] = "";
};
int scene_kills(SceneKill* out, int max);
bool scene_can_undo();
bool scene_can_redo();
void scene_undo();
void scene_redo();
void effects_game_frame(const TheaterState& state);
// Where the picture is taken from this frame.
void camera_view_origin(float out[3]);
void camera_view(float origin[3], float axis[9], float tan[2]);
int scene_lights(SceneLight* out, int max);
void scene_set_light(int index, const SceneLight& light);
int scene_add_light();
int scene_add_light_at(const float origin[3]);
void camera_pick(float u, float v);
int camera_picked(float point[3], float normal[3]);
void scene_move_light_here(int index);
void scene_remove_light(int index);
BoneState camera_bone_state();
void camera_set_bone(const BoneCamera& bone);
CameraState camera_state();
void camera_set_mode(CameraMode mode);
void camera_set_speed(float speed);
void camera_set_fov(float fov);
void camera_add_point();
void camera_clear_points();
// The player's own body drawn (the game's third person), for the cameras away from the eyes.
void camera_set_body(bool on);
void camera_set_clean(bool on);
void camera_set_player_aim(bool on);
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

// sky.cpp: the sky's picture changed (hue in degrees, saturation and brightness as factors) and turned
// (degrees around the engine's up, then the two axes across).
struct SkyState
{
	bool known = false, failed = false, changed = false;
	float hsv[3] = {0.0f, 1.0f, 1.0f}, turn[3] = {};
	char picture[64] = "";   // the saved sky it is drawn from ("": the map's own)
	char message[160] = "";  // what the last save or load came to
};

// A map's sky saved (theater\\skies) so it can be the sky of other maps.
void sky_save();
void sky_use(const char* name);
int sky_saved(char names[][64], int max);
SkyState sky_state();
void sky_set(const float hsv[3], const float turn[3]);
void sky_reset();
void sky_put_back_now();
struct ID3D11Device;
struct ID3D11DeviceContext;
void sky_frame(ID3D11Device* device, ID3D11DeviceContext* context, const TheaterState& state);
// The images of the loaded map's skies (the map's name into world): how many, or less than 1 if none.
int theater_sky_images(void** out, int max, char* world, size_t size);
