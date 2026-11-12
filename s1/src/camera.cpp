// S1iris' cameras: a free camera flown with the keys and mouse, and a path through points placed with it
// that the camera follows as the recording plays (a dolly camera).
//
// The game works its view out once a frame (CG_CalcViewValues, 0x1401B7BB0, called from
// CG_DrawActiveFrame) and then draws by it. The view is in the client game's "refdef" at 0x1415575C0:
// a viewport (8 bytes), the time and frame time, then tan(half the horizontal field of view) at +0x10,
// the vertical one at +0x14, the eye's origin at +0x18 and its axis (forward, left, up) at +0x24. The
// origin is copied there after the calculation from 0x141557610, so the camera's is written to both.
// The field of view is set later in the frame, so it is put in at the frame's next step.
#include "theater.hpp"

#include <MinHook.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
	constexpr uintptr_t CG_CALC_VIEW_VALUES = 0x1401B7BB0, CG_RENDER_SCENE = 0x1401DF410, REFDEF = 0x1415575C0, CG_GET_FOV = 0x1401D59D0;
	constexpr uintptr_t REFDEF_TAN_X = 0x1415575D0, REFDEF_TAN_Y = 0x1415575D4, REFDEF_ORIGIN = 0x1415575D8,
		REFDEF_AXIS = 0x1415575E4, VIEW_ORIGIN = 0x141557610;
	constexpr uintptr_t DVAR_FIND_VAR = 0x1404BF8B0;
	constexpr float PI = 3.14159265358979f, TO_RAD = PI / 180.0f, TO_DEG = 180.0f / PI;

	using calc_view_t = void (*)(int);
	calc_view_t g_original_calc_view = nullptr;
	// the scene's drawing: (client, refdef, ...), its further arguments passed on as they came
	using render_scene_t = void (*)(int, void*, void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
	render_scene_t g_original_render_scene = nullptr;
	// the field of view the game means to draw with (cg_fov and aiming down the sights worked in):
	// float (client), 0x1401D59D0, what the game's CG_CalcFov (0x1401D3F80) makes the view's tans from
	using get_fov_t = float (*)(int);
	get_fov_t g_original_get_fov = nullptr;
	std::atomic<float> g_game_fov{65.0f};
	std::atomic<bool> g_body{true};
	bool g_body_set = false;

	struct Pose
	{
		float origin[3] = {};
		float angles[3] = {};  // pitch, yaw, roll
		float fov = 80.0f;
	};

	struct Point
	{
		std::uint32_t ms = 0;
		Pose pose;
	};

	std::mutex g_lock;  // the pose and the points: the game's thread flies, the panel's reads and edits
	Pose g_pose;
	std::vector<Point> g_points;
	std::atomic<bool> g_free{false}, g_follow{false};
	std::atomic<int> g_mode{static_cast<int>(CameraMode::FirstPerson)};
	int g_view_state = -1;  // what the game's third person and gun settings were last set for
	std::atomic<float> g_speed{400.0f};
	std::atomic<bool> g_add_request{false}, g_clear_request{false};
	bool g_have_pose = false, g_overriding = false, g_logged_view = false;
	bool g_f2_was = false, g_k_was = false, g_l_was = false;
	LARGE_INTEGER g_last_tick{};
	std::atomic<unsigned> g_bad_view{0};

	void log(const char* format, ...)
	{
		char text[512];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		theater_log(text);
	}

	template <typename T> T& at(const uintptr_t address)
	{
		return *reinterpret_cast<T*>(address);
	}

	bool down(const int key)
	{
		return (GetAsyncKeyState(key) & 0x8000) != 0;
	}

	// The game's own: forward, left, up from pitch, yaw, roll.
	void angles_to_axis(const float* angles, float axis[3][3])
	{
		const float sp = std::sin(angles[0] * TO_RAD), cp = std::cos(angles[0] * TO_RAD);
		const float sy = std::sin(angles[1] * TO_RAD), cy = std::cos(angles[1] * TO_RAD);
		const float sr = std::sin(angles[2] * TO_RAD), cr = std::cos(angles[2] * TO_RAD);
		axis[0][0] = cp * cy;
		axis[0][1] = cp * sy;
		axis[0][2] = -sp;
		axis[1][0] = sr * sp * cy - cr * sy;
		axis[1][1] = sr * sp * sy + cr * cy;
		axis[1][2] = sr * cp;
		axis[2][0] = cr * sp * cy + sr * sy;
		axis[2][1] = cr * sp * sy - sr * cy;
		axis[2][2] = cr * cp;
	}

	bool view_is_sane()
	{
		const float tan_x = at<float>(REFDEF_TAN_X), tan_y = at<float>(REFDEF_TAN_Y);
		const float* axis = &at<float>(REFDEF_AXIS);
		const float forward = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
		const float up = axis[6] * axis[6] + axis[7] * axis[7] + axis[8] * axis[8];
		return tan_x > 0.01f && tan_x < 60.0f && tan_y > 0.01f && tan_y < 60.0f && forward > 0.9f && forward < 1.1f && up > 0.9f && up < 1.1f;
	}

	// Where the game's own view is, to start flying from.
	Pose pose_of_game_view()
	{
		Pose pose;
		memcpy(pose.origin, &at<float>(REFDEF_ORIGIN), sizeof(pose.origin));
		const float* forward = &at<float>(REFDEF_AXIS);
		pose.angles[1] = std::atan2(forward[1], forward[0]) * TO_DEG;
		pose.angles[0] = std::atan2(-forward[2], std::sqrt(forward[0] * forward[0] + forward[1] * forward[1])) * TO_DEG;
		pose.angles[2] = 0.0f;
		pose.fov = g_game_fov;
		return pose;
	}

	void set_dvar_bool(const char* name, const bool value)
	{
		const auto find = reinterpret_cast<void* (*)(const char*)>(DVAR_FIND_VAR);
		if (auto* dvar = static_cast<std::uint8_t*>(find(name)))
		{
			dvar[0x10] = value ? 1 : 0;
		}
		else
		{
			static std::atomic<int> noted{0};
			if (noted++ < 4)
			{
				log("camera: the game has no setting called %s", name);
			}
		}
	}

	// With the camera away from the player's eyes: the player's body is drawn and the gun in the hands
	// of the camera is not.
	// (0.24 switched both when the free camera came on, and the game stopped the match with an error at
	// that moment; until the log says which of the two it was, neither is touched.)
	void set_outside_view(const bool third_person, const bool gun)
	{
		(void)third_person;
		(void)gun;
	}

	// The camera on the path at a time of the recording (in milliseconds, with the part of one). The
	// points are in time order and their angles have been made to run on without a jump of a full turn.
	//
	// Between two points the path is a cubic that leaves the first and arrives at the second at the
	// speeds the points have, and a point's speed is that from the point before it to the one after,
	// over the time between those two. So the camera's speed has no jump at a point however unevenly
	// the points are spaced in time. (0.23 to 0.30 took each stretch as a Catmull-Rom curve run through
	// evenly: with one stretch a second long and the next a tenth, the camera sped up tenfold at the point.)
	Pose sample_path(const std::vector<Point>& points, const double ms)
	{
		if (ms <= points.front().ms)
		{
			return points.front().pose;
		}

		if (ms >= points.back().ms)
		{
			return points.back().pose;
		}

		size_t i = 0;
		while (i + 2 < points.size() && points[i + 1].ms <= ms)
		{
			++i;
		}

		const Point& a = points[i];
		const Point& b = points[i + 1];
		const Point& before = i > 0 ? points[i - 1] : a;
		const Point& after = i + 2 < points.size() ? points[i + 2] : b;
		const float span = static_cast<float>(b.ms - a.ms);
		if (span <= 0.0f)
		{
			return b.pose;
		}

		const float t = static_cast<float>((ms - a.ms) / span);
		const float t2 = t * t, t3 = t2 * t;
		const float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
		const float span_a = static_cast<float>(b.ms - before.ms), span_b = static_cast<float>(after.ms - a.ms);
		auto curve = [&](const float p0, const float p1, const float p2, const float p3) {
			// the speeds at a and at b, per millisecond
			const float speed_a = span_a > 0.0f ? (p2 - p0) / span_a : 0.0f;
			const float speed_b = span_b > 0.0f ? (p3 - p1) / span_b : 0.0f;
			return h00 * p1 + h10 * span * speed_a + h01 * p2 + h11 * span * speed_b;
		};

		Pose pose;
		for (int k = 0; k < 3; ++k)
		{
			pose.origin[k] = curve(before.pose.origin[k], a.pose.origin[k], b.pose.origin[k], after.pose.origin[k]);
			pose.angles[k] = curve(before.pose.angles[k], a.pose.angles[k], b.pose.angles[k], after.pose.angles[k]);
		}

		pose.fov = curve(before.pose.fov, a.pose.fov, b.pose.fov, after.pose.fov);
		return pose;
	}

	void add_point(const std::uint32_t ms)
	{
		// under g_lock
		Point point{ms, g_pose};
		size_t i = 0;
		while (i < g_points.size() && g_points[i].ms < ms)
		{
			++i;
		}

		if (i < g_points.size() && g_points[i].ms == ms)
		{
			g_points[i] = point;
		}
		else
		{
			g_points.insert(g_points.begin() + i, point);
		}

		for (size_t n = 1; n < g_points.size(); ++n)
		{
			for (int k = 0; k < 3; ++k)
			{
				float& angle = g_points[n].pose.angles[k];
				const float before = g_points[n - 1].pose.angles[k];
				while (angle - before > 180.0f)
				{
					angle -= 360.0f;
				}

				while (angle - before < -180.0f)
				{
					angle += 360.0f;
				}
			}
		}
	}

	// Flies the free camera: W A S D, E up, Q down, Shift fast, Ctrl slow, the mouse wheel rolls (up to
	// the right), the mouse looks while the game has it (not the panel).
	void fly(const float dt)
	{
		if (GetForegroundWindow() != ui_window() || theater_keys_blocked())
		{
			return;
		}

		static bool panel_had_mouse = false;
		const bool panel_has_mouse = ui_mouse_is_panels();
		if (!panel_has_mouse)
		{
			int dx = 0, dy = 0;
			ui_take_mouse_motion(&dx, &dy);
			if (panel_had_mouse)
			{
				// the pointer was wherever the panel left it
				dx = dy = 0;
			}

			const float sensitivity = 0.05f * (g_pose.fov / 80.0f);
			g_pose.angles[1] -= dx * sensitivity;
			g_pose.angles[0] += dy * sensitivity;
			g_pose.angles[0] = g_pose.angles[0] > 89.0f ? 89.0f : g_pose.angles[0] < -89.0f ? -89.0f : g_pose.angles[0];
		}

		panel_had_mouse = panel_has_mouse;
		float axis[3][3];
		angles_to_axis(g_pose.angles, axis);
		const float speed = g_speed * (down(VK_SHIFT) ? 3.0f : 1.0f) * (down(VK_CONTROL) ? 0.25f : 1.0f) * dt;
		const float forward = (down('W') ? 1.0f : 0.0f) - (down('S') ? 1.0f : 0.0f);
		const float left = (down('A') ? 1.0f : 0.0f) - (down('D') ? 1.0f : 0.0f);
		const float up = (down('E') ? 1.0f : 0.0f) - (down('Q') ? 1.0f : 0.0f);
		for (int k = 0; k < 3; ++k)
		{
			g_pose.origin[k] += (axis[0][k] * forward + axis[1][k] * left) * speed;
		}

		g_pose.origin[2] += up * speed;
		g_pose.angles[2] += ui_take_mouse_wheel() * 3.0f;
	}

	void apply(const Pose& pose)
	{
		float axis[3][3];
		angles_to_axis(pose.angles, axis);
		memcpy(&at<float>(REFDEF_AXIS), axis, sizeof(axis));
		memcpy(&at<float>(REFDEF_ORIGIN), pose.origin, sizeof(pose.origin));
		memcpy(&at<float>(VIEW_ORIGIN), pose.origin, sizeof(pose.origin));
	}

	Pose g_applied;  // this frame's, for the field of view at the frame's next step

	void hooked_calc_view(const int client)
	{
		g_original_calc_view(client);

		const TheaterState state = theater_state();
		const bool focus = state.playing && GetForegroundWindow() == ui_window() && !theater_keys_blocked();
		const CameraMode mode_now = static_cast<CameraMode>(g_mode.load());
		const bool f2 = focus && down(VK_F2);
		if (f2 && !g_f2_was)
		{
			g_mode = static_cast<int>(mode_now == CameraMode::Free ? CameraMode::FirstPerson : CameraMode::Free);
		}

		g_f2_was = f2;
		const bool k = focus && g_free && down('K');
		if (k && !g_k_was)
		{
			g_add_request = true;
		}

		g_k_was = k;
		const bool l = focus && down('L');
		if (l && !g_l_was)
		{
			g_mode = static_cast<int>(mode_now == CameraMode::Dolly ? CameraMode::Free : CameraMode::Dolly);
		}

		g_l_was = l;
		if (!state.playing)
		{
			g_mode = static_cast<int>(CameraMode::FirstPerson);
		}

		const CameraMode mode = static_cast<CameraMode>(g_mode.load());

		LARGE_INTEGER now{}, frequency{};
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&frequency);
		float dt = g_last_tick.QuadPart ? static_cast<float>(now.QuadPart - g_last_tick.QuadPart) / static_cast<float>(frequency.QuadPart) : 0.0f;
		g_last_tick = now;
		dt = dt > 0.1f ? 0.1f : dt;

		std::lock_guard<std::mutex> lock(g_lock);
		if (g_clear_request.exchange(false))
		{
			g_points.clear();
		}

		// the path needs two points; with fewer the dolly camera stays where the camera was left
		const bool follow = state.playing && mode == CameraMode::Dolly && g_points.size() >= 2;
		const bool parked = state.playing && mode == CameraMode::Dolly && !follow && g_have_pose;
		const bool free_camera = state.playing && mode == CameraMode::Free;
		const bool overriding = follow || free_camera || parked;
		const int view_state = !state.playing ? 0 : overriding ? 2 : mode == CameraMode::ThirdPerson ? 1 : 0;
		if (view_state != g_view_state)
		{
			g_view_state = view_state;
			set_outside_view(view_state != 0, view_state != 2);
		}

		if (overriding && !view_is_sane())
		{
			// not the view this was written for: leave the game's alone
			if (g_bad_view++ < 3)
			{
				log("camera: the game's view does not look as expected (tan %g %g); the camera is left alone", at<float>(REFDEF_TAN_X),
				    at<float>(REFDEF_TAN_Y));
			}

			g_free = g_follow = false;
			return;
		}

		// the player's body, where it was asked for and the camera is away from the eyes
		const bool body = state.playing && g_body && (overriding || mode == CameraMode::ThirdPerson);
		if (body != g_body_set)
		{
			g_body_set = body;
			theater_command(body ? "camera_thirdPerson 1" : "camera_thirdPerson 0");
		}

		const bool was_overriding = g_overriding;
		if (overriding != g_overriding)
		{
			g_overriding = overriding;
			ui_camera_has_mouse(free_camera);
			if (!g_logged_view)
			{
				g_logged_view = true;
				const float* o = &at<float>(REFDEF_ORIGIN);
				const float* a = &at<float>(REFDEF_AXIS);
				log("camera: the game's view: tan %g %g, origin %.1f %.1f %.1f, forward %.3f %.3f %.3f, up %.3f %.3f %.3f", at<float>(REFDEF_TAN_X),
				    at<float>(REFDEF_TAN_Y), o[0], o[1], o[2], a[0], a[1], a[2], a[6], a[7], a[8]);
			}

			log("camera: %s", follow ? "following the path" : free_camera ? "free" : "the player's own again");
		}
		else if (free_camera != g_free)
		{
			ui_camera_has_mouse(free_camera);
		}

		if (free_camera && (!g_free || !g_have_pose))
		{
			// start where the camera already is, or at the player's eyes
			if (!was_overriding || !g_have_pose)
			{
				g_pose = pose_of_game_view();
			}

			g_have_pose = true;
			int dx, dy;
			ui_take_mouse_motion(&dx, &dy);
		}

		g_free = free_camera;
		g_follow = follow;
		if (free_camera)
		{
			fly(dt);
			if (g_add_request.exchange(false))
			{
				add_point(state.position_ms);
				log("camera: point %zu of the path at %.1f s", g_points.size(), state.position_ms / 1000.0);
			}
		}
		else
		{
			g_add_request = false;
		}

		if (follow)
		{
			g_pose = sample_path(g_points, state.position_exact);
			g_have_pose = true;
		}

		if (overriding)
		{
			g_applied = g_pose;
			apply(g_pose);
		}
	}

	// The game works the eye's place out again from the player on its way to drawing the scene (in
	// CG_DrawActive, 0x1401DC450: 0.23 to 0.26 wrote the camera only after CG_CalcViewValues, and the
	// picture stayed the player's). So the camera goes in last, where the scene is handed over to be
	// drawn (0x1401DF410, called with the refdef).
	void hooked_render_scene(const int client, void* refdef, void* a3, const std::uint64_t a4, const std::uint64_t a5, const std::uint64_t a6,
	                         const std::uint64_t a7, const std::uint64_t a8)
	{
		if (g_overriding && refdef == reinterpret_cast<void*>(REFDEF) && view_is_sane())
		{
			apply(g_applied);
			static bool noted = false;
			if (!noted)
			{
				noted = true;
				log("camera: put into the scene's drawing");
			}
		}

		g_original_render_scene(client, refdef, a3, a4, a5, a6, a7, a8);
	}

	float hooked_get_fov(const int client)
	{
		const float game = g_original_get_fov(client);
		g_game_fov = game;
		if (g_overriding)
		{
			const float fov = g_applied.fov;
			return fov < 1.0f ? 1.0f : fov > 160.0f ? 160.0f : fov;
		}

		return game;
	}
}

CameraState camera_state()
{
	CameraState state;
	state.mode = static_cast<CameraMode>(g_mode.load());
	state.speed = g_speed;
	state.body = g_body;
	std::lock_guard<std::mutex> lock(g_lock);
	state.fov = g_pose.fov;
	state.points = static_cast<int>(g_points.size());
	for (size_t i = 0; i < g_points.size() && i < 64; ++i)
	{
		state.point_ms[i] = g_points[i].ms;
	}

	return state;
}

void camera_set_mode(const CameraMode mode) { g_mode = static_cast<int>(mode); }
void camera_set_speed(const float speed) { g_speed = speed; }
void camera_add_point() { g_add_request = true; }
void camera_set_body(const bool on) { g_body = on; }

void camera_playback_ended()
{
	if (g_body_set)
	{
		g_body_set = false;
		theater_command("camera_thirdPerson 0");
	}
}
void camera_clear_points() { g_clear_request = true; }

void camera_set_fov(const float fov)
{
	std::lock_guard<std::mutex> lock(g_lock);
	g_pose.fov = fov;
}

void camera_start()
{
	MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(CG_CALC_VIEW_VALUES), reinterpret_cast<LPVOID>(&hooked_calc_view),
	                                 reinterpret_cast<LPVOID*>(&g_original_calc_view));
	if (status == MH_OK)
	{
		status = MH_CreateHook(reinterpret_cast<LPVOID>(CG_RENDER_SCENE), reinterpret_cast<LPVOID>(&hooked_render_scene),
		                       reinterpret_cast<LPVOID*>(&g_original_render_scene));
	}

	if (status == MH_OK)
	{
		status = MH_CreateHook(reinterpret_cast<LPVOID>(CG_GET_FOV), reinterpret_cast<LPVOID>(&hooked_get_fov),
		                       reinterpret_cast<LPVOID*>(&g_original_get_fov));
	}

	if (status == MH_OK)
	{
		status = MH_EnableHook(reinterpret_cast<LPVOID>(CG_GET_FOV));
	}

	if (status == MH_OK)
	{
		status = MH_EnableHook(reinterpret_cast<LPVOID>(CG_RENDER_SCENE));
	}

	if (status == MH_OK)
	{
		status = MH_EnableHook(reinterpret_cast<LPVOID>(CG_CALC_VIEW_VALUES));
	}

	log(status == MH_OK ? "camera: the game's view hooked. While a recording plays: F2 free camera, K a point of the path, L the dolly camera"
	                    : "camera: the game's view NOT hooked (%s)",
	    MH_StatusToString(status));
}
