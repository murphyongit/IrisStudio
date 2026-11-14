// S1iris: a theater mode for Advanced Warfare multiplayer (S1x).
//
// Advanced Warfare has no demo system. A demo is the stream of messages the server sends the client,
// kept in a file and given to the client again later (as the Ghosts demo system of anomaly/iw6-mod and
// H1MVM's port of it do).
//
// Recording: CL_ParseServerMessage is hooked and every message it is given is written to
// theater\rec_<date>_<time>.awdm, with the map and game type beside it in <file>.txt.
//
// Playback (first form: from the start, at normal speed, nothing else): F9 at the menu starts the newest
// recording's map as a local match, so that the game loads everything a match needs; when the client is
// ready for its first snapshot (CA_PRIMED) the local server's messages are dropped and the recorded ones
// are parsed in their place, at the times they came, and nothing is sent to the server any more. F10, or
// the recording's end, disconnects.
//
// Addresses are those of s1_mp64_ship.exe (linker timestamp 1563218830), found in its unpacked code; those
// marked s1-mod are from that client's symbols.

#include "theater.hpp"
#include "passes.hpp"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <share.h>
#include <mutex>
#include <string>
#include <cctype>
#include <utility>
#include <vector>

namespace
{
	constexpr char VERSION[] = "0.68 (recorder, playback, IrisStudio's editor)";

	// void CL_ParseServerMessage(int localClientNum, msg_t* msg)
	constexpr uintptr_t CL_PARSE_SERVER_MESSAGE = 0x1402129B0;
	// its first bytes once the game has unpacked itself: mov [rsp+10h],rbx ; mov [rsp+18h],rdi ; push rbp
	constexpr unsigned char PROLOGUE[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55};

	// void CL_WritePacket(int localClientNum) (s1-mod): the client's packet to the server, every frame
	constexpr uintptr_t CL_WRITE_PACKET = 0x1402058F0;
	// The player's own position is not in the server's messages: the client keeps what it predicted for
	// each of its commands in a ring (cl->clientArchive, 256 of 0xA4 bytes at cl+0x74EC: the command's
	// server time, origin, velocity, bob cycle, movement direction; the next index at cl+0x118EC), and a
	// snapshot's player state is given the entry of its command's time.
	// void CL_SavePredictedPlayerInformationForServerTime(clientActive_t* cl, int serverTime): writes an entry
	constexpr uintptr_t CL_SAVE_PREDICTED = 0x140213140;
	// bool CL_GetPredictedPlayerInformationForServerTime(clientActive_t* cl, int serverTime, playerState_s* ps)
	constexpr uintptr_t CL_GET_PREDICTED = 0x140210FF0;
	// what the writer reads of cl, and what the reader writes of the player state
	constexpr size_t CL_ORIGIN = 0x5228, CL_VELOCITY = 0x5234, CL_BOB_CYCLE = 0x52C0, CL_MOVEMENT_DIR = 0x52C4;
	constexpr size_t PS_BOB_CYCLE = 0x0E, PS_ORIGIN = 0x78, PS_VELOCITY = 0x84, PS_MOVEMENT_DIR = 0xD0;
	// int CG_DrawActiveFrame(int localClientNum, int serverTime, int demoType, int cubemapShot, int cubemapSize,
	// int renderScreen, ...): its demoType (kept at 0x1414C66E8) is what tells the game's own code a demo is
	// being watched: the view is the recorded player's, and the viewer's mouse and keys are not his
	constexpr uintptr_t CG_DRAW_ACTIVE_FRAME = 0x1401D4710;
	constexpr int DEMO_TYPE_CLIENT = 1;
	// int Com_TimeScaleMsec(int msec): a frame's real milliseconds made the game's, once a frame; never less
	// than 1, so the game cannot be slowed far or stopped by its own time scale
	constexpr uintptr_t COM_TIME_SCALE_MSEC = 0x1403D1E20;
	// void CG_GameMessage(int localClientNum, const char* message) (s1-mod): a line in the game's message feed
	constexpr uintptr_t CG_GAME_MESSAGE = 0x1401A3050;
	constexpr int CA_ACTIVE = 10;
	// IrisStudio's steps of the timescale (core/src/Components/Playback.hpp)
	constexpr float SPEEDS[] = {0.001f, 0.005f, 0.01f, 0.025f, 0.05f, 0.1f, 0.125f, 0.2f, 0.25f, 0.333f,
	                            0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 5.0f, 10.0f, 20.0f, 50.0f};
	constexpr int NORMAL_SPEED = 12;
	constexpr std::uint32_t SKIP_MS = 10000;
	// What going back in a recording has to undo, for the game to take the recording's first messages
	// again: the client's and the client game's places in the stream of snapshots. (CL_SetCGameTime
	// 0x1401FD810, CL_GetSnapshot 0x1401FC800, CG_ProcessSnapshots 0x1401C17E0.)
	// cl->snap (cl+8): valid, serverTime, messageNum
	constexpr uintptr_t CL_SNAP_VALID = 0x1417E6A70, CL_SNAP_SERVER_TIME = 0x1417E6A78, CL_SNAP_MESSAGE_NUM = 0x1417E6A7C;
	// cl->serverTime (never set lower by the game), oldServerTime, oldFrameServerTime (a snapshot older than
	// it is an error), serverTimeDelta (serverTime is it plus cls.realtime), newSnapshots
	constexpr uintptr_t CL_SERVER_TIME = 0x1417E6ABC, CL_OLD_SERVER_TIME = 0x1417E6AC8, CL_OLD_FRAME_SERVER_TIME = 0x1417E6ACC;
	constexpr uintptr_t CL_SERVER_TIME_DELTA = 0x1417E6AD0, CL_NEW_SNAPSHOTS = 0x1417E6AD4;
	// cl->snapshots: their count, the ring, an entry's size and its valid flag
	constexpr uintptr_t CL_SNAPSHOT_COUNT = 0x1417F3240, CL_SNAPSHOTS = 0x1417F3318;
	constexpr size_t CL_SNAPSHOT_SIZE = 0x5160, CL_SNAPSHOT_VALID = 0x5118;
	// the client game's: the newest snapshot's number and time it has seen, the snapshot it is at and the
	// one it is going to (pointers), and the number of the last one it has read
	constexpr uintptr_t CG_LATEST_SNAPSHOT_NUM = 0x1414C66FC, CG_LATEST_SNAPSHOT_TIME = 0x1414C6700;
	constexpr uintptr_t CG_SNAP = 0x1414C6708, CG_NEXT_SNAP = 0x1414C6710, CG_PROCESSED_SNAPSHOT_NUM = 0x141646C9C;
	constexpr std::uint32_t BACK_MS = 10000;
	// Messages more than this behind playback's clock are a jump's (a skip, or going back): they are given
	// at the frame's start, as fast as the game takes them.
	constexpr std::uint32_t BEHIND_MS = 250;
	// The client's clock in playback. The game sets its clock from the snapshots as they come: a
	// millisecond or two forward or back at each one, and running on past the newest for a few
	// milliseconds when the next is late. In a live match that cannot be seen; in slow motion every
	// snapshot (20 a second) showed as a hitch. A recording knows what comes: its messages are given
	// LEAD_MS before they came, and the client's clock is set each frame to the playback clock plus what
	// separates a snapshot's time from when it came (the largest seen: the one least delayed), so it is
	// always between two snapshots and moves evenly.
	constexpr std::uint32_t LEAD_MS = 150;
	// void SV_Frame(int msec): the local server's step, from the game's frame
	constexpr uintptr_t SV_FRAME = 0x1404425B0;
	// gentity_s* SV_AddBot(const char* name), gentity_s* SV_AddTestClient(int) (s1-mod)
	constexpr uintptr_t SV_ADD_BOT = 0x140438EC0;
	constexpr uintptr_t SV_ADD_TEST_CLIENT = 0x140439190;
	// void Cbuf_AddText(int localClientNum, const char* text) (s1-mod)
	constexpr uintptr_t CBUF_ADD_TEXT = 0x1403AED70;
	// dvar_t* Dvar_FindVar(const char* name) (s1-mod); a dvar's current value is at +0x10 (after its name,
	// its flags, and its type and modified flag of a byte each)
	constexpr uintptr_t DVAR_FIND_VAR = 0x1404BF8B0;
	// every dvar, sorted by name: how many, and the pointers to them (s1-mod's symbols). A dvar: its
	// name, flags at +8, type at +0xC, the current value at +0x10, the latched one at +0x20, the one it
	// resets to at +0x30, its limits at +0x40 (s1-mod's structs).
	constexpr uintptr_t DVAR_COUNT = 0x14B32AA30, SORTED_DVARS = 0x14B32AA50;
	// CG_SetFrameInterpolation: how far the client game is between its two snapshots, from whole
	// milliseconds (stored at 0x141557578)
	constexpr uintptr_t CG_SET_FRAME_INTERPOLATION = 0x140193B50, CG_FRAME_INTERPOLATION = 0x141557578;
	constexpr size_t CG_SNAPSHOT_SERVER_TIME = 0x5B70;
	constexpr size_t DVAR_CURRENT = 0x10;
	// clientUIActive.connectionState (s1-mod's rcon.cpp; CL_PacketEvent tests it)
	constexpr uintptr_t CONNECTION_STATE = 0x1417E1690;
	constexpr int CA_CONNECTING = 3, CA_LOADING = 8, CA_PRIMED = 9;
	// the virtual lobby (the menu's own level) is loaded (s1-mod)
	constexpr uintptr_t VIRTUAL_LOBBY_LOADED = 0x1417E161D;
	// What CL_PacketEvent (0x14020D720) sets from a packet's first two numbers before it has the message
	// parsed: the message's sequence (snapshots name the ones they are changes from by it) and the last of
	// the client's commands the server has; and the times it keeps for its timeout.
	constexpr uintptr_t CLC_SERVER_MESSAGE_SEQUENCE = 0x141C9AA24;
	constexpr uintptr_t CLC_RELIABLE_ACKNOWLEDGE = 0x141C7AA20;
	constexpr uintptr_t CLC_RELIABLE_SEQUENCE = 0x141C7AA1C;
	constexpr uintptr_t CLC_LAST_PACKET_TIME = 0x141C7A8F8;
	constexpr uintptr_t CLS_REALTIME = 0x141CDC030;

	// what is read of the game's msg_t
	constexpr size_t MSG_SIZE = 0x38;
	constexpr size_t MSG_DATA = 0x08;
	constexpr size_t MSG_CURSIZE = 0x1C;
	constexpr size_t MSG_READCOUNT = 0x24;
	constexpr size_t MSG_OVERFLOWED = 0x00;
	constexpr size_t MSG_USE_ZLIB = 0x30;
	// the two numbers CL_PacketEvent has read of a message when it is parsed
	constexpr std::int32_t MSG_HEADER = 8;
	constexpr unsigned MAX_MESSAGE = 0x20000;
	// A record that is not a message has its kind where a message has its client (0): the player's
	// predicted position for one command.
	constexpr std::uint32_t KIND_PREDICTED = 0x44455250;
	// The player's own view direction is not in the server's messages either (a snapshot's player state has
	// none for the player it is sent to). With each predicted position the recording keeps the part of cl
	// around the positions (cl+0x5200, 0xE0 bytes), which has cl->viewangles at cl+0x52D0; and once a
	// second, as another kind of record, the start of the player state a snapshot was read into (0x120
	// bytes), which has what the server adds to those angles.
	constexpr size_t CL_BLOCK = 0x5200, CL_BLOCK_SIZE = 0xE0, CL_VIEWANGLES = 0x52D0;
	constexpr std::uint32_t KIND_PLAYER_STATE = 0x41545350;
	// A player state's delta_angles (+0x90: what the server adds to the client's own angles, set where the
	// player spawns) and viewangles (+0x1B4: the two added, what the view is drawn by when a demo is
	// watched; 0 in a snapshot for the player it is sent to). Found in a recording's own data (0.17) and
	// in the game's functions that set both (0x1404012F0, 0x1403508D6).
	constexpr size_t PLAYER_STATE_SIZE = 0x120, PS_DELTA_ANGLES = 0x90, PS_VIEWANGLES = 0x1B4;
	constexpr int HOOK_DELAY_S = 25;
	// the match's map is surely the game's by this many messages (2 s)
	constexpr std::uint64_t MATCH_INFO_AT = 40;

	// a pause this long between messages is a new match: a new file
	constexpr ULONGLONG NEW_FILE_GAP_MS = 5000;

	bool read_bytes_forward(uintptr_t address, void* out, size_t count);

	char g_folder[MAX_PATH] = {};
	CRITICAL_SECTION g_lock;
	FILE* g_log = nullptr;
	FILE* g_file = nullptr;
	ULONGLONG g_file_start = 0, g_last_message = 0, g_last_flush = 0, g_last_report = 0;
	std::uint64_t g_file_messages = 0, g_file_bytes = 0, g_faults = 0;
	std::atomic<bool> g_started{false};
	std::atomic<std::uint64_t> g_calls{0};

	using parse_t = void (*)(int, void*);
	parse_t g_original_parse = nullptr;
	using write_packet_t = void (*)(int);
	write_packet_t g_original_write_packet = nullptr;

	// One recorded message.
	struct Record
	{
		std::uint32_t ms = 0;
		std::int32_t client = 0;
		unsigned char head[MSG_SIZE] = {};
		std::vector<unsigned char> data;
	};

	// What the client predicted of its player for the command of a server time.
	struct Predicted
	{
		std::int32_t serverTime = 0;
		float origin[3] = {};
		float velocity[3] = {};
		std::int32_t bobCycle = 0;
		std::int32_t movementDir = 0;
	};

	static_assert(sizeof(Predicted) == 36);

	// A predicted position as playback keeps it: with the view direction, where the recording has one.
	struct PredictedView : Predicted
	{
		float viewangles[3] = {};
		bool hasView = false;
	};

	enum class Play
	{
		Idle,
		// the map has been asked for and is loading
		Starting,
		// the recording is what the client is given
		Playing,
	};

	// all of this is the main thread's, but for the two requests, which the key thread makes
	std::atomic<Play> g_play{Play::Idle};
	std::vector<Record> g_records;
	// the recording's predicted positions, in the order of their times
	std::vector<PredictedView> g_predicted;
	// the game's own demo mode while a recording plays (F8)
	std::atomic<bool> g_demo_mode{true};
	// Playback's clock: the game's own milliseconds since the recording took over, as playback's
	// Com_TimeScaleMsec gave them out, so that the messages come at the pace the game's time runs at.
	std::atomic<std::uint64_t> g_clock{0};
	float g_clock_fraction = 0.0f;
	std::atomic<int> g_speed{NORMAL_SPEED};
	std::atomic<bool> g_paused{false};
	// the key thread's requests, taken on the main thread
	std::atomic<bool> g_pause_request{false}, g_slower_request{false}, g_faster_request{false}, g_skip_request{false};
	std::atomic<bool> g_back_request{false}, g_restart_request{false}, g_rebuild_request{false};
	// what the panel asks for: a place in the recording (ms; -1 for none), and how long the recording is
	std::atomic<std::int64_t> g_seek_request{-1};
	std::atomic<std::uint32_t> g_length_ms{0};
	std::uint64_t g_playback_number = 0;
	// every message due by the clock has been given (after a jump the picture takes some frames to arrive)
	std::atomic<bool> g_settled{false};
	// recording to video: the clock goes on by exactly a frame of this rate each frame the game draws,
	// however long the frame took (0: not recording)
	std::atomic<float> g_capture_fps{0.0f};
	char g_shown_name[96] = {}, g_shown_map[64] = {};
	HMODULE g_module = nullptr;
	// a snapshot has to be given at once, whatever the clock says (after going back: the client has none)
	bool g_need_snapshot = false;
	// the client's time is to be set to the next snapshot's, once one has been given after going back
	bool g_resync_time = false;
	constexpr unsigned OFFSETS = 64;
	std::int32_t g_offsets[OFFSETS] = {};
	unsigned g_offset_count = 0;
	bool g_have_time_offset = false;
	std::int32_t g_time_offset = 0, g_last_snapshot_time = 0, g_pin_error = 0;
	// A jump with time frozen. What the game shows of a moment is not that moment's snapshots alone: where
	// an animation is in its cycle, and the smoothing of the player's own position (cg_errordecay), come
	// from the frames before, and with time frozen they never catch up. The same moment looked different
	// reached from before and from after (0.33). So such a jump goes to ROLL_MS before its time, always by
	// way of the recording's start, and plays up to it at ROLL_SPEED: the same moment is then reached the
	// same way every time. The position shown meanwhile is the one being gone to.
	constexpr std::uint32_t ROLL_MS = 3000;
	constexpr float ROLL_SPEED = 8.0f;
	std::atomic<std::int64_t> g_roll_to{-1};
	bool g_caught_up = false;
	// a recording asked for by name (the editor's list), and when it may start
	char g_wanted_name[128] = {};
	std::atomic<bool> g_wanted{false};
	ULONGLONG g_wanted_after = 0;
	std::atomic<std::uint64_t> g_predicted_given{0}, g_predicted_missed{0};
	size_t g_next_record = 0;
	ULONGLONG g_play_start = 0, g_play_asked = 0, g_play_report = 0;
	bool g_feeding = false;
	std::uint64_t g_dropped = 0;
	std::atomic<bool> g_play_request{false}, g_stop_request{false};
	// the client was seen connecting to the match asked for (the key thread watches: nothing of the main
	// thread's runs here while a map loads)
	std::atomic<bool> g_seen_loading{false};
	CRITICAL_SECTION g_play_lock;
	std::atomic<std::uint64_t> g_bots_refused{0}, g_server_frames_skipped{0};
	std::atomic<unsigned> g_menus_kept_shut{0};
	char g_play_name[MAX_PATH] = {};

	int connection_state()
	{
		int state = 0;
		read_bytes_forward(CONNECTION_STATE, &state, sizeof(state));
		return state;
	}

	void log_line(const char* fmt, ...)
	{
		if (!g_log)
		{
			return;
		}

		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
		va_list args;
		va_start(args, fmt);
		vfprintf(g_log, fmt, args);
		va_end(args);
		fputc('\n', g_log);
		fflush(g_log);
	}

	bool read_bytes(const uintptr_t address, void* out, const size_t count)
	{
		__try
		{
			memcpy(out, reinterpret_cast<const void*>(address), count);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool read_bytes_forward(const uintptr_t address, void* out, const size_t count)
	{
		return read_bytes(address, out, count);
	}

	// A dvar's text, or "" (on the main thread).
	void read_dvar(const char* name, char* text, const size_t size)
	{
		text[0] = 0;
		__try
		{
			const auto find = reinterpret_cast<void* (*)(const char*)>(DVAR_FIND_VAR);
			if (const auto* dvar = static_cast<const unsigned char*>(find(name)))
			{
				const char* value = nullptr;
				memcpy(&value, dvar + DVAR_CURRENT, sizeof(value));
				if (value)
				{
					strncpy_s(text, size, value, _TRUNCATE);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			text[0] = 0;
		}
	}

	std::string dvar_string(const char* name)
	{
		char text[64];
		read_dvar(name, text, sizeof(text));
		return text;
	}

	void command(const char* text)
	{
		__try
		{
			reinterpret_cast<void (*)(int, const char*)>(CBUF_ADD_TEXT)(0, text);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	// A line on screen, where the game shows its own messages (main thread, in a match).
	void show(const char* text)
	{
		log_line("shown: %s", text);
		// (during playback they came up in the kill feed: the editor has the same on its own panel)
		if (g_play != Play::Idle)
		{
			return;
		}

		__try
		{
			if (connection_state() == CA_ACTIVE)
			{
				reinterpret_cast<void (*)(int, const char*)>(CG_GAME_MESSAGE)(0, text);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	char g_file_path[MAX_PATH] = {};

	// The map and game type of the match being recorded, beside the recording: what playback starts.
	void write_match_info()
	{
		const auto map = dvar_string("mapname");
		const auto type = dvar_string("g_gametype");
		if (map.empty())
		{
			log_line("recording: the game would not say its map; this recording cannot be played back");
			return;
		}

		char path[MAX_PATH];
		sprintf_s(path, "%s.txt", g_file_path);
		FILE* info = nullptr;
		if (fopen_s(&info, path, "w") == 0 && info)
		{
			fprintf(info, "map %s\ngametype %s\n", map.c_str(), type.c_str());
			fclose(info);
		}

		log_line("recording: map %s, game type %s", map.c_str(), type.c_str());
	}

	void finish_clip();

	void close_file()
	{
		if (g_file)
		{
			// (a named recording still under way ends with the match)
			finish_clip();
			fclose(g_file);
			g_file = nullptr;
			log_line("recording closed: %llu messages, %llu bytes, %.1f s", g_file_messages, g_file_bytes,
			         (g_last_message - g_file_start) / 1000.0);
		}
	}

	bool open_file(const ULONGLONG now)
	{
		SYSTEMTIME t;
		GetLocalTime(&t);
		char path[MAX_PATH];
		sprintf_s(path, "%s\\rec_%04d%02d%02d_%02d%02d%02d.awdm", g_folder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
		          t.wSecond);
		strcpy_s(g_file_path, path);
		// (shared: a named recording is cut from it while the match is still being recorded)
		g_file = _fsopen(path, "wb", _SH_DENYNO);
		if (!g_file)
		{
			log_line("could not create %s", path);
			return false;
		}

		// header: "AWDM", format 2 (1 had messages only), the size of the msg_t kept with each message
		const std::uint32_t header[3] = {0x4D445741, 2, static_cast<std::uint32_t>(MSG_SIZE)};
		fwrite(header, sizeof(header), 1, g_file);
		g_file_start = now;
		g_file_messages = 0;
		g_file_bytes = 0;
		log_line("recording started: %s", path);
		return true;
	}

	// One record: ms since the file began, the client, the msg_t as the game had it, then its data.
	void record(const int client, const void* msg)
	{
		unsigned char head[MSG_SIZE];
		if (!msg || !read_bytes(reinterpret_cast<uintptr_t>(msg), head, sizeof(head)))
		{
			++g_faults;
			return;
		}

		uintptr_t data = 0;
		std::int32_t cursize = 0;
		memcpy(&data, head + MSG_DATA, sizeof(data));
		memcpy(&cursize, head + MSG_CURSIZE, sizeof(cursize));
		if (!data || cursize <= 0 || static_cast<unsigned>(cursize) > MAX_MESSAGE)
		{
			++g_faults;
			return;
		}

		static unsigned char buffer[MAX_MESSAGE];
		if (!read_bytes(data, buffer, cursize))
		{
			++g_faults;
			return;
		}

		const ULONGLONG now = GetTickCount64();
		if (g_file && now - g_last_message > NEW_FILE_GAP_MS)
		{
			close_file();
		}

		if (!g_file && !open_file(now))
		{
			return;
		}

		const std::uint32_t record_head[3] = {static_cast<std::uint32_t>(now - g_file_start), static_cast<std::uint32_t>(client),
		                                      static_cast<std::uint32_t>(cursize)};
		fwrite(record_head, sizeof(record_head), 1, g_file);
		fwrite(head, sizeof(head), 1, g_file);
		fwrite(buffer, 1, cursize, g_file);
		++g_file_messages;
		g_file_bytes += cursize;
		if (g_file_messages == MATCH_INFO_AT)
		{
			write_match_info();
		}

		g_last_message = now;
		if (now - g_last_flush > 1000)
		{
			fflush(g_file);
			g_last_flush = now;
		}

		if (now - g_last_report > 10000)
		{
			g_last_report = now;
			std::int32_t readcount = 0;
			memcpy(&readcount, head + MSG_READCOUNT, sizeof(readcount));
			log_line("recording: %llu messages, %llu bytes so far (last: %d bytes, read from %d, unreadable so far %llu)",
			         g_file_messages, g_file_bytes, cursize, readcount, g_faults);
		}
	}

	bool load_recording(const char* path, std::string& map, std::string& type)
	{
		char info_path[MAX_PATH];
		sprintf_s(info_path, "%s.txt", path);
		FILE* info = nullptr;
		if (fopen_s(&info, info_path, "r") != 0 || !info)
		{
			return false;
		}

		char key[32], value[64];
		while (fscanf_s(info, "%31s %63s", key, static_cast<unsigned>(sizeof(key)), value, static_cast<unsigned>(sizeof(value))) == 2)
		{
			(strcmp(key, "map") == 0 ? map : type) = value;
		}

		fclose(info);
		FILE* file = nullptr;
		if (map.empty() || fopen_s(&file, path, "rb") != 0 || !file)
		{
			return false;
		}

		g_records.clear();
		g_predicted.clear();
		std::uint32_t header[3] = {};
		bool good = fread(header, sizeof(header), 1, file) == 1 && header[0] == 0x4D445741 && header[2] == MSG_SIZE;
		while (good)
		{
			std::uint32_t head[3];
			Record record;
			if (fread(head, sizeof(head), 1, file) != 1 || head[2] == 0 || head[2] > MAX_MESSAGE)
			{
				break;
			}

			if (head[1] == KIND_PREDICTED)
			{
				PredictedView predicted;
				unsigned char block[CL_BLOCK_SIZE];
				if ((head[2] != sizeof(Predicted) && head[2] != sizeof(Predicted) + sizeof(block)) ||
				    fread(static_cast<Predicted*>(&predicted), sizeof(Predicted), 1, file) != 1)
				{
					break;
				}

				if (head[2] > sizeof(Predicted))
				{
					if (fread(block, sizeof(block), 1, file) != 1)
					{
						break;
					}

					memcpy(predicted.viewangles, block + (CL_VIEWANGLES - CL_BLOCK), sizeof(predicted.viewangles));
					predicted.hasView = true;
				}

				if (g_predicted.empty() || predicted.serverTime > g_predicted.back().serverTime)
				{
					g_predicted.push_back(predicted);
				}

				continue;
			}

			if (head[1] == KIND_PLAYER_STATE)
			{
				// for study only
				if (fseek(file, static_cast<long>(head[2]), SEEK_CUR) != 0)
				{
					break;
				}

				continue;
			}

			if (fread(record.head, sizeof(record.head), 1, file) != 1)
			{
				break;
			}

			record.ms = head[0];
			record.client = static_cast<std::int32_t>(head[1]);
			record.data.resize(head[2]);
			if (fread(record.data.data(), 1, head[2], file) != head[2])
			{
				break;
			}

			g_records.push_back(std::move(record));
		}

		fclose(file);
		return good && !g_records.empty();
	}

	// The newest recording that has its match's map beside it.
	bool find_newest(char* path)
	{
		char pattern[MAX_PATH];
		sprintf_s(pattern, "%s\\rec_*.awdm.txt", g_folder);
		WIN32_FIND_DATAA found;
		const HANDLE search = FindFirstFileA(pattern, &found);
		if (search == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		char newest[MAX_PATH] = {};
		do
		{
			if (strcmp(found.cFileName, newest) > 0)
			{
				strcpy_s(newest, found.cFileName);
			}
		} while (FindNextFileA(search, &found));
		FindClose(search);
		newest[strlen(newest) - 4] = 0;
		sprintf_s(path, MAX_PATH, "%s\\%s", g_folder, newest);
		return true;
	}

	// Console commands asked for from the editor's thread, run at a frame's start.
	std::mutex g_commands_lock;
	std::vector<std::string> g_commands;

	void run_commands()
	{
		std::vector<std::string> commands;
		{
			std::lock_guard<std::mutex> lock(g_commands_lock);
			commands.swap(g_commands);
		}

		for (const std::string& text : commands)
		{
			log_line("command: %s", text.c_str());
			command((text + "\n").c_str());
		}
	}

	void dump_dvars();

	void stop_playback(const char* why)
	{
		if (g_play == Play::Idle)
		{
			return;
		}

		log_line("playback ended (%s): %zu of %zu messages given, %llu of the local match's dropped", why, g_next_record,
		         g_records.size(), g_dropped);
		g_play = Play::Idle;
		g_records.clear();
		g_records.shrink_to_fit();
		g_predicted.clear();
		// (the map is let go of next: the sky's picture is the game's own again first)
		sky_put_back_now();
		command("disconnect\n");
	}

	void start_playback()
	{
		if (g_play != Play::Idle)
		{
			return;
		}

		if (g_file)
		{
			log_line("playback: a match is being recorded; leave it first");
			return;
		}

		std::string map, type;
		bool found = false;
		if (g_wanted_name[0])
		{
			sprintf_s(g_play_name, "%s\\%s", g_folder, g_wanted_name);
			g_wanted_name[0] = 0;
			found = true;
		}
		else
		{
			found = find_newest(g_play_name);
		}

		if (!found || !load_recording(g_play_name, map, type))
		{
			log_line("playback: no recording with its map beside it (one recorded with this version) was found");
			return;
		}

		g_predicted_given = 0;
		g_predicted_missed = 0;
		g_length_ms = g_records.empty() ? 0 : g_records.back().ms;
		++g_playback_number;
		g_roll_to = -1;
		g_have_time_offset = false;
		g_last_snapshot_time = 0;
		g_offset_count = 0;
		{
			const char* slash = strrchr(g_play_name, '\\');
			strncpy_s(g_shown_name, slash ? slash + 1 : g_play_name, _TRUNCATE);
			strncpy_s(g_shown_map, map.c_str(), _TRUNCATE);
		}
		g_seek_request = -1;
		log_line("playback: %s, %zu messages, %zu of the player's positions, %.1f s, map %s, game type %s; connection state %d, lobby %d",
		         g_play_name, g_records.size(), g_predicted.size(), g_records.back().ms / 1000.0, map.c_str(), type.c_str(),
		         connection_state(),
		         static_cast<int>(*reinterpret_cast<const unsigned char*>(VIRTUAL_LOBBY_LOADED)));
		char text[256];
		sprintf_s(text, "g_gametype %s; ui_gametype %s; map %s\n", type.empty() ? "war" : type.c_str(),
		          type.empty() ? "war" : type.c_str(), map.c_str());
		command(text);
		g_seen_loading = false;
		g_play = Play::Starting;
		g_play_asked = GetTickCount64();
		g_next_record = 0;
		g_dropped = 0;
	}

	// Gives the client one recorded message as CL_PacketEvent gives it one from the network.
	void feed(Record& record)
	{
		unsigned char msg[MSG_SIZE];
		memcpy(msg, record.head, sizeof(msg));
		static unsigned char buffer[MAX_MESSAGE];
		memcpy(buffer, record.data.data(), record.data.size());
		const void* data = buffer;
		memcpy(msg + MSG_DATA, &data, sizeof(data));
		const std::int32_t zero = 0, header = MSG_HEADER;
		memcpy(msg + MSG_OVERFLOWED, &zero, sizeof(zero));
		memcpy(msg + MSG_READCOUNT, &header, sizeof(header));
		std::int32_t sequence = 0;
		memcpy(&sequence, record.data.data(), sizeof(sequence));
		*reinterpret_cast<std::int32_t*>(CLC_SERVER_MESSAGE_SEQUENCE) = sequence;
		// the server of the recording has every command of this client's: none is to be sent again
		*reinterpret_cast<std::int32_t*>(CLC_RELIABLE_ACKNOWLEDGE) = *reinterpret_cast<std::int32_t*>(CLC_RELIABLE_SEQUENCE);
		*reinterpret_cast<std::int32_t*>(CLC_LAST_PACKET_TIME) = *reinterpret_cast<std::int32_t*>(CLS_REALTIME);
		g_feeding = true;
		g_original_parse(record.client, msg);
		g_feeding = false;
	}

	bool feed_guarded(Record& record)
	{
		__try
		{
			feed(record);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			g_feeding = false;
			return false;
		}
	}

	// faults noted so far (a few at most after each start and each going back)
	std::atomic<int> g_faults_noted{0};
	// What the game's place in the recording looks like, for the log.
	void log_stream(const char* when)
	{
		__try
		{
			log_line("  %s: connection state %d; client snapshot valid %d, number %d, time %d; client time %d, delta %d, new snapshots %d; "
			         "client game latest %d, read %d, at %p, going to %p, its time %d",
			         when, connection_state(), *reinterpret_cast<std::int32_t*>(CL_SNAP_VALID),
			         *reinterpret_cast<std::int32_t*>(CL_SNAP_MESSAGE_NUM), *reinterpret_cast<std::int32_t*>(CL_SNAP_SERVER_TIME),
			         *reinterpret_cast<std::int32_t*>(CL_SERVER_TIME), *reinterpret_cast<std::int32_t*>(CL_SERVER_TIME_DELTA),
			         *reinterpret_cast<std::int32_t*>(CL_NEW_SNAPSHOTS), *reinterpret_cast<std::int32_t*>(CG_LATEST_SNAPSHOT_NUM),
			         *reinterpret_cast<std::int32_t*>(CG_PROCESSED_SNAPSHOT_NUM), *reinterpret_cast<void**>(CG_SNAP),
			         *reinterpret_cast<void**>(CG_NEXT_SNAP), *reinterpret_cast<std::int32_t*>(0x141557580));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	// The view angles of the snapshots the client game is at and is going to, for the log: whether the
	// recording's player turns in them.
	void log_view()
	{
		__try
		{
			const auto* at = *reinterpret_cast<const unsigned char**>(CG_SNAP);
			const auto* next = *reinterpret_cast<const unsigned char**>(CG_NEXT_SNAP);
			if (at && next)
			{
				float a[2], n[2];
				memcpy(a, at + PS_VIEWANGLES, sizeof(a));
				memcpy(n, next + PS_VIEWANGLES, sizeof(n));
				log_line("  view: at snapshot of time %d, pitch %.1f yaw %.1f; going to time %d, pitch %.1f yaw %.1f; demo type the game has %d",
				         *reinterpret_cast<const std::int32_t*>(at + 0x5B70), a[0], a[1], *reinterpret_cast<const std::int32_t*>(next + 0x5B70),
				         n[0], n[1], *reinterpret_cast<const std::int32_t*>(0x1414C66E8));
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}

	// ticks after going back whose state is logged
	int g_log_ticks = 0;

	// Takes the client and the client game back to before the recording's first message, so that the
	// recording can be given again from its start (its first snapshot is a whole one, not a change).
	bool reset_stream()
	{
		__try
		{
			*reinterpret_cast<std::int32_t*>(CL_SNAP_VALID) = 0;
			*reinterpret_cast<std::int32_t*>(CL_SNAP_MESSAGE_NUM) = 0;
			*reinterpret_cast<std::int32_t*>(CL_NEW_SNAPSHOTS) = 0;
			*reinterpret_cast<std::int32_t*>(CL_OLD_FRAME_SERVER_TIME) = 0;
			const std::int32_t count = *reinterpret_cast<std::int32_t*>(CL_SNAPSHOT_COUNT);
			for (std::int32_t i = 0; i < count && count <= 4096; ++i)
			{
				*reinterpret_cast<std::int32_t*>(CL_SNAPSHOTS + i * CL_SNAPSHOT_SIZE + CL_SNAPSHOT_VALID) = 0;
			}

			*reinterpret_cast<std::int32_t*>(CG_LATEST_SNAPSHOT_NUM) = 0;
			*reinterpret_cast<std::int32_t*>(CG_LATEST_SNAPSHOT_TIME) = 0;
			// (The client game's own snapshots are left to the game: the recording begins with the
			// match's game state, at which the game restarts the client game itself.)
			*reinterpret_cast<std::int32_t*>(CG_PROCESSED_SNAPSHOT_NUM) = 0;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	// The client's time put at the snapshot it now has (the game itself never sets it back).
	void resync_time()
	{
		__try
		{
			if (*reinterpret_cast<std::int32_t*>(CL_SNAP_VALID))
			{
				const std::int32_t time = *reinterpret_cast<std::int32_t*>(CL_SNAP_SERVER_TIME);
				*reinterpret_cast<std::int32_t*>(CL_SERVER_TIME_DELTA) = time - *reinterpret_cast<std::int32_t*>(CLS_REALTIME);
				*reinterpret_cast<std::int32_t*>(CL_SERVER_TIME) = time;
				*reinterpret_cast<std::int32_t*>(CL_OLD_SERVER_TIME) = time;
				*reinterpret_cast<std::int32_t*>(CL_OLD_FRAME_SERVER_TIME) = 0;
				g_resync_time = false;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			g_resync_time = false;
		}
	}

	// Playback to a time of the recording before the one it is at: from the start again, the messages up to
	// that time given as fast as the game takes them.
	std::atomic<std::uint32_t> g_rewinds{0};

	void go_back_to(const std::uint64_t ms)
	{
		++g_rewinds;
		// (the HUD's script takes down the kill feed's lines and the popups left from later in the recording)
		static int flushes = 0;
		char flush[48];
		snprintf(flush, sizeof(flush), "set s1iris_hud_flush %d", ++flushes);
		theater_command(flush);
		theater_hud_wake();
		g_faults_noted = 0;
		log_stream("before going back");
		g_log_ticks = 12;
		if (!reset_stream())
		{
			log_line("playback: going back failed (a fault resetting the game's place in the recording)");
			return;
		}

		g_next_record = 0;
		g_clock = ms;
		g_clock_fraction = 0.0f;
		g_need_snapshot = true;
		g_resync_time = true;
		log_line("playback: back to %.1f s", ms / 1000.0);
	}

	void jump_to(const std::uint64_t target)
	{
		if (!g_paused)
		{
			g_roll_to = -1;
			if (target < g_clock)
			{
				go_back_to(target);
			}
			else
			{
				g_clock = target;
			}

			return;
		}

		go_back_to(target > ROLL_MS ? target - ROLL_MS : 0);
		g_caught_up = false;
		g_roll_to = static_cast<std::int64_t>(target);
	}

	// Playback's step. On the main thread whenever the client has a message from the local match or would
	// send its packet; from the key thread (main false) only what cannot wait for those: a start or a stop
	// while nothing is being played.
	void play_step(const bool main, const bool frame_start)
	{
		if (main && !frame_start)
		{
			// in the drawing of a frame: only the messages of a match under way
			if (g_play != Play::Playing || g_feeding)
			{
				return;
			}
		}

		if (g_stop_request && (main ? frame_start : g_play != Play::Playing))
		{
			g_stop_request = false;
			stop_playback("F10");
		}

		if (g_wanted && g_play == Play::Idle && GetTickCount64() >= g_wanted_after)
		{
			g_wanted = false;
			g_play_request = false;
			start_playback();
		}

		if (g_play_request && g_play == Play::Idle)
		{
			g_play_request = false;
			start_playback();
		}

		g_play_request = false;
		if (g_play == Play::Idle || g_feeding)
		{
			return;
		}

		const ULONGLONG now = GetTickCount64();
		const int state = connection_state();
		const bool lobby = *reinterpret_cast<const unsigned char*>(VIRTUAL_LOBBY_LOADED) != 0;
		if (g_play == Play::Starting)
		{
			if (state >= CA_CONNECTING && state <= CA_LOADING && !lobby)
			{
				g_seen_loading = true;
			}

			if (g_seen_loading && main && state >= CA_PRIMED)
			{
				g_clock = 0;
				g_clock_fraction = 0.0f;
				g_speed = NORMAL_SPEED;
				g_paused = false;
				g_pause_request = g_slower_request = g_faster_request = g_skip_request = false;
				g_back_request = g_restart_request = false;
				g_need_snapshot = false;
				g_resync_time = false;
				g_play = Play::Playing;
				g_play_start = now;
				g_play_report = now;
				log_line("playback: the client is ready (connection state %d); the recording is its server from here", state);
			dump_dvars();
			}
			else if (now - g_play_asked > 120000 || (!g_seen_loading && now - g_play_asked > 30000))
			{
				log_line("playback: the match never became ready (connection state %d, lobby %d, seen loading %d)", state,
				         lobby ? 1 : 0, g_seen_loading ? 1 : 0);
				g_play = Play::Idle;
				g_records.clear();
			}

			return;
		}

		if (!main)
		{
			return;
		}

		if (state < CA_PRIMED)
		{
			stop_playback("the client left the match");
			return;
		}

		if (frame_start)
		{

			if (g_pause_request.exchange(false))
			{
				g_paused = !g_paused;
				show(g_paused ? "S1iris: paused" : "S1iris: playing");
			}

			const bool slower = g_slower_request.exchange(false), faster = g_faster_request.exchange(false);
			if (slower != faster)
			{
				const int count = static_cast<int>(sizeof(SPEEDS) / sizeof(SPEEDS[0]));
				const int speed = g_speed + (faster ? 1 : -1);
				g_speed = speed < 0 ? 0 : speed >= count ? count - 1 : speed;
				char text[64];
				sprintf_s(text, "S1iris: speed %gx", SPEEDS[g_speed]);
				show(text);
			}

			// (from where a jump under way is going to, if there is one)
			const std::int64_t rolling = g_roll_to;
			const std::uint64_t place = rolling >= 0 ? static_cast<std::uint64_t>(rolling) : g_clock.load();
			if (g_skip_request.exchange(false))
			{
				jump_to(place + SKIP_MS);
				show("S1iris: 10 seconds on");
			}

			if (g_back_request.exchange(false))
			{
				jump_to(place > BACK_MS ? place - BACK_MS : 0);
				show("S1iris: 10 seconds back");
			}

			if (const std::int64_t seek = g_seek_request.exchange(-1); seek >= 0)
			{
				jump_to(static_cast<std::uint64_t>(seek));
			}

			if (g_rebuild_request.exchange(false))
			{
				if (g_paused)
				{
					jump_to(place);
				}
				else
				{
					g_roll_to = -1;
					go_back_to(place);
				}
			}

			if (g_restart_request.exchange(false))
			{
				g_roll_to = -1;
				go_back_to(0);
				show("S1iris: from the start");
			}

			if (g_log_ticks > 0)
			{
				--g_log_ticks;
				log_stream("tick");
			}
		}

		const auto elapsed = static_cast<std::uint32_t>(g_clock.load());
		// Where the messages are given. In a match under way: where the client would send its packet, in
		// the game's drawing of a frame, the place the game has a server's messages at itself when it
		// draws (0.14 and 0.15 gave them at the frame's start, and the view no longer turned). At the
		// frame's start: what cannot be given there. The recording's first messages restart the client
		// game, which the drawing does not survive (going back), and before the first snapshot there is
		// no drawing to give them in.
		const std::uint32_t due = elapsed + LEAD_MS;
		const bool behind = g_next_record < g_records.size() && g_records[g_next_record].ms + BEHIND_MS <= due;
		const bool at_start = state != CA_ACTIVE || g_need_snapshot || behind;
		if (frame_start)
		{
			g_settled = state == CA_ACTIVE && !g_need_snapshot && g_seek_request < 0 &&
				(g_next_record >= g_records.size() || g_records[g_next_record].ms > due);
		}

		if (frame_start == at_start)
		{
			for (int given = 0; given < 100 && g_next_record < g_records.size(); ++given)
			{
				if (g_records[g_next_record].ms > due && !(g_need_snapshot && !*reinterpret_cast<const std::int32_t*>(CL_SNAP_VALID)))
				{
					break;
				}

				if (!feed_guarded(g_records[g_next_record]))
				{
					log_line("playback: message %zu could not be parsed (a fault in the game)", g_next_record);
					stop_playback("a message the game could not take");
					return;
				}

				// what separates this snapshot's time from when it came
				if (*reinterpret_cast<const std::int32_t*>(CL_SNAP_VALID))
				{
					const std::int32_t time = *reinterpret_cast<const std::int32_t*>(CL_SNAP_SERVER_TIME);
					if (time != g_last_snapshot_time)
					{
						g_last_snapshot_time = time;
						// the largest of the last few seconds' (the least delayed), and only taken when it
						// is well off the one in use: a server that fell behind is followed, the jitter is not
						const std::int32_t offset = time - static_cast<std::int32_t>(g_records[g_next_record].ms);
						g_offsets[g_offset_count++ % OFFSETS] = offset;
						std::int32_t largest = offset;
						for (unsigned i = 0; i < g_offset_count && i < OFFSETS; ++i)
						{
							largest = g_offsets[i] > largest ? g_offsets[i] : largest;
						}

						if (!g_have_time_offset || largest - g_time_offset > 8 || g_time_offset - largest > 8)
						{
							g_time_offset = largest;
							g_have_time_offset = true;
						}
					}
				}

				++g_next_record;
				if (g_resync_time)
				{
					resync_time();
				}
			}

			if (*reinterpret_cast<const std::int32_t*>(CL_SNAP_VALID))
			{
				g_need_snapshot = false;
			}
		}

		if (!frame_start)
		{
			return;
		}

		g_caught_up = state == CA_ACTIVE && !g_need_snapshot &&
			(g_next_record >= g_records.size() || g_records[g_next_record].ms + BEHIND_MS > due);

		if (now - g_play_report > 5000)
		{
			g_play_report = now;
			log_stream("now");
			log_view();
			log_line("playback: %.0f s (speed %gx%s), %zu of %zu messages given, connection state %d, %llu local messages dropped, "
			         "%llu bots refused, %llu server steps skipped; the player's position given %llu times, not found %llu; demo mode %s",
			         elapsed / 1000.0, SPEEDS[g_speed], g_paused ? ", paused" : "", g_next_record, g_records.size(),
			         connection_state(), g_dropped, g_bots_refused.load(),
			         g_server_frames_skipped.load(), g_predicted_given.load(), g_predicted_missed.load(),
			         g_demo_mode ? "on" : "off");
			log_line("playback: team and class menu openings kept shut: %u; the client's clock is %d ms from where it is set (snapshots %d ms from when they came)",
			         g_menus_kept_shut.load(), g_pin_error, g_have_time_offset ? g_time_offset : 0);
		}

		if (g_next_record >= g_records.size() && elapsed >= g_records.back().ms)
		{
			stop_playback("the recording's end");
		}
	}

	void play_tick(const bool main = true, const bool frame_start = true)
	{
		EnterCriticalSection(&g_play_lock);
		play_step(main, frame_start);
		LeaveCriticalSection(&g_play_lock);
	}

	void hooked_parse(const int client, void* msg)
	{
		++g_calls;
		if (g_feeding)
		{
			g_original_parse(client, msg);
			return;
		}

		if (g_play == Play::Playing || (g_play == Play::Starting && g_seen_loading && connection_state() >= CA_PRIMED))
		{
			// the local match's: the recording is the server now (or is about to be, at the next frame's
			// start: the local match's first snapshot is not to come before the recording's)
			++g_dropped;
			return;
		}

		if (g_play == Play::Idle)
		{
			// The menu's own level (the virtual lobby) is a connection too, with messages of its own 20
			// times a second: a match's recording ends where it begins, and it is not recorded.
			static bool was_lobby = false;
			const bool lobby = *reinterpret_cast<const unsigned char*>(VIRTUAL_LOBBY_LOADED) != 0;
			EnterCriticalSection(&g_lock);
			if (lobby != was_lobby)
			{
				was_lobby = lobby;
				log_line(lobby ? "the menu's lobby is loaded: not recording" : "the menu's lobby is gone: recording what comes");
				if (lobby)
				{
					close_file();
				}
			}

			if (!lobby)
			{
				record(client, msg);
			}

			LeaveCriticalSection(&g_lock);
		}

		g_original_parse(client, msg);
	}

	void hooked_write_packet(const int client)
	{
		play_tick(true, false);
		if (g_play == Play::Playing)
		{
			// nothing of the viewer's goes to the local match
			return;
		}

		g_original_write_packet(client);
	}

	// The match that playback starts is only there to have the game load a match's map: nothing of it is to
	// happen. Its bots would take weapons, and a weapon with attachments gets its number where it is first
	// taken, in one table the local server and the client share: the recording's numbers for the same
	// weapons then differ ("Custom weapon index mismatch"). So it gets no bots, and once the recording is
	// the client's server it does not run at all.
	using sv_frame_t = void (*)(int);
	sv_frame_t g_original_sv_frame = nullptr;
	using add_bot_t = void* (*)(const char*);
	add_bot_t g_original_add_bot = nullptr;
	using add_test_client_t = void* (*)(int);
	add_test_client_t g_original_add_test_client = nullptr;

	void hooked_sv_frame(const int msec)
	{
		if (g_play == Play::Playing)
		{
			++g_server_frames_skipped;
			return;
		}

		g_original_sv_frame(msec);
	}

	void* hooked_add_bot(const char* name)
	{
		if (g_play != Play::Idle)
		{
			++g_bots_refused;
			return nullptr;
		}

		return g_original_add_bot(name);
	}

	void* hooked_add_test_client(const int index)
	{
		if (g_play != Play::Idle)
		{
			++g_bots_refused;
			return nullptr;
		}

		return g_original_add_test_client(index);
	}

	using time_scale_msec_t = int (*)(int);
	time_scale_msec_t g_original_time_scale_msec = nullptr;

	// While a recording plays, the game's time is playback's: a frame's milliseconds times the speed, none
	// when paused, the fractions kept; and what is given out is what playback's clock counts.
	// The client's clock for the frame that is starting: the playback clock plus what separates the
	// snapshots' time from when they came. The game adds the frame's milliseconds (what this hook hands
	// out) to its own running time and its clock is that plus a difference it keeps; the difference is
	// set so the sum is the clock wanted. Going back (a jump), its clock is put back as well: on its own
	// it never runs backwards.
	void pin_client_time(const int frame_ms)
	{
		if (!g_have_time_offset || g_need_snapshot || g_resync_time || connection_state() != CA_ACTIVE ||
		    !*reinterpret_cast<const std::int32_t*>(CL_SNAP_VALID))
		{
			return;
		}

		const std::int32_t wanted = static_cast<std::int32_t>(g_clock.load()) + g_time_offset;
		const std::int32_t real = *reinterpret_cast<const std::int32_t*>(CLS_REALTIME);
		auto* client_time = reinterpret_cast<std::int32_t*>(CL_SERVER_TIME);
		// (how far the last frame's clock ended up from the one set for it less this frame's step: 0 if
		// the game's time is this hook's, as assumed)
		g_pin_error = *client_time - (wanted - frame_ms);
		*reinterpret_cast<std::int32_t*>(CL_SERVER_TIME_DELTA) = wanted - (real + frame_ms);
		if (wanted < *client_time)
		{
			*client_time = wanted;
			*reinterpret_cast<std::int32_t*>(CL_OLD_SERVER_TIME) = wanted;
		}
	}

	// ---- named recordings, from the console: "record <name>" in a private match, "stoprecord", and
	// "demo <name>" to play one. A match is recorded from its start anyway (a recording has to begin
	// with the match's game state), so a named recording is cut from the match's: everything before
	// "record" was typed is kept, with its times set to 0, so that playback gives it to the game at once
	// and begins where the recording was asked for; what came after "stoprecord" is left out.
	constexpr std::uintptr_t CMD_ADD_COMMAND = 0x1403AF2C0, CMD_ARGS = 0x1479ECB00;
	char g_clip_name[64] = {};
	std::uint32_t g_clip_start = 0;
	bool g_clip = false;

	// The console command's argument (1 is the first after its name), or "".
	const char* command_argument(const int index)
	{
		const auto* args = reinterpret_cast<const unsigned char*>(CMD_ARGS);
		const int nesting = *reinterpret_cast<const int*>(args);
		if (nesting < 0 || nesting >= 8 || index >= *reinterpret_cast<const int*>(args + 0x44 + nesting * 4))
		{
			return "";
		}

		const char* const* argv = *reinterpret_cast<const char* const* const*>(args + 0x68 + nesting * 8);
		return argv && argv[index] ? argv[index] : "";
	}

	// A name a file can have: letters, digits, _ and -.
	bool clean_name(const char* text, char* out, const size_t size)
	{
		size_t length = 0;
		for (; text[length] && length + 1 < size; ++length)
		{
			const char c = text[length];
			out[length] = isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
		}

		out[length] = 0;
		// (given with its ending: without)
		if (length > 5 && _stricmp(out + length - 5, "_awdm") == 0)
		{
			out[length - 5] = 0;
		}

		return out[0] != 0;
	}

	bool save_clip(const std::uint32_t end_ms)
	{
		char source[MAX_PATH];
		EnterCriticalSection(&g_lock);
		if (g_file)
		{
			fflush(g_file);
		}

		strcpy_s(source, g_file_path);
		LeaveCriticalSection(&g_lock);
		char path[MAX_PATH], info_from[MAX_PATH], info_to[MAX_PATH];
		sprintf_s(path, "%s\\%s.awdm", g_folder, g_clip_name);
		sprintf_s(info_from, "%s.txt", source);
		sprintf_s(info_to, "%s.txt", path);
		FILE* in = _fsopen(source, "rb", _SH_DENYNO);
		FILE* out = nullptr;
		if (!in || fopen_s(&out, path, "wb") != 0 || !out)
		{
			if (in)
			{
				fclose(in);
			}

			return false;
		}

		std::uint32_t header[3] = {}, head[3] = {};
		std::vector<unsigned char> payload;
		unsigned kept = 0;
		bool good = fread(header, sizeof(header), 1, in) == 1 && fwrite(header, sizeof(header), 1, out) == 1;
		while (good && fread(head, sizeof(head), 1, in) == 1 && head[2] != 0 && head[2] <= MAX_MESSAGE && head[0] <= end_ms)
		{
			const bool message = head[1] != KIND_PREDICTED && head[1] != KIND_PLAYER_STATE;
			payload.resize(head[2] + (message ? MSG_SIZE : 0));
			if (fread(payload.data(), 1, payload.size(), in) != payload.size())
			{
				break;
			}

			head[0] = head[0] > g_clip_start ? head[0] - g_clip_start : 0;
			good = fwrite(head, sizeof(head), 1, out) == 1 && fwrite(payload.data(), 1, payload.size(), out) == payload.size();
			++kept;
		}

		fclose(in);
		fclose(out);
		good = good && kept > 0 && CopyFileA(info_from, info_to, FALSE);
		log_line("recording %s: %u records kept, from %.1f s to %.1f s of the match%s", g_clip_name, kept, g_clip_start / 1000.0,
		         end_ms == UINT32_MAX ? (g_last_message - g_file_start) / 1000.0 : end_ms / 1000.0, good ? "" : " (NOT written whole)");
		return good;
	}

	void finish_clip()
	{
		if (g_clip)
		{
			g_clip = false;
			save_clip(UINT32_MAX);
		}
	}

	void command_record()
	{
		char name[64];
		if (!g_file || g_play != Play::Idle)
		{
			show("S1iris: start a private match first, then record");
			log_line("record: no match is being recorded");
			return;
		}

		if (!clean_name(command_argument(1), name, sizeof(name)))
		{
			SYSTEMTIME t;
			GetLocalTime(&t);
			sprintf_s(name, "demo_%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
		}

		strcpy_s(g_clip_name, name);
		g_clip_start = static_cast<std::uint32_t>(GetTickCount64() - g_file_start);
		g_clip = true;
		char text[128];
		sprintf_s(text, "S1iris: recording %s (stoprecord ends it)", name);
		show(text);
		log_line("record: %s from %.1f s of the match", name, g_clip_start / 1000.0);
	}

	void command_stoprecord()
	{
		if (!g_clip)
		{
			show("S1iris: nothing is being recorded by name");
			return;
		}

		g_clip = false;
		const bool good = save_clip(static_cast<std::uint32_t>(GetTickCount64() - g_file_start));
		char text[160];
		sprintf_s(text, good ? "S1iris: %s saved (demo %s plays it)" : "S1iris: %s could not be saved", g_clip_name, g_clip_name);
		show(text);
	}

	void command_demo()
	{
		char name[64], path[MAX_PATH];
		if (!clean_name(command_argument(1), name, sizeof(name)))
		{
			log_line("demo: which one? (demo <name>)");
			return;
		}

		sprintf_s(path, "%s\\%s.awdm", g_folder, name);
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
		{
			show("S1iris: there is no recording of that name");
			log_line("demo: %s is not there", path);
			return;
		}

		strcat_s(name, ".awdm");
		theater_play(name);
	}

	void command_demo_restart()
	{
		if (g_play == Play::Playing)
		{
			g_restart_request = true;
		}
		else
		{
			log_line("demo_restart: no recording is playing");
		}
	}

	// (once, on the game's own thread: its list of commands is its own)
	void add_commands()
	{
		static unsigned char storage[8][0x40];
		static const struct
		{
			const char* name;
			void (*function)();
		} COMMANDS[] = {{"record", command_record}, {"stoprecord", command_stoprecord}, {"demo", command_demo},
		                {"iris_record", command_record}, {"iris_stoprecord", command_stoprecord}, {"iris_demo", command_demo},
		                {"demo_restart", command_demo_restart}, {"iris_demo_restart", command_demo_restart}};
		__try
		{
			for (int i = 0; i < 8; ++i)
			{
				reinterpret_cast<void (*)(const char*, void (*)(), void*)>(CMD_ADD_COMMAND)(COMMANDS[i].name, COMMANDS[i].function, storage[i]);
			}

			log_line("commands: record <name>, stoprecord, demo <name>, demo_restart (and the same with iris_ before them)");
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			log_line("commands: could NOT be added");
		}
	}

	int hooked_time_scale_msec(const int msec)
	{
		static bool commands_added = false;
		if (!commands_added)
		{
			commands_added = true;
			add_commands();
		}

		// Playback's step is taken here, at the start of the game's frame and nowhere else. The game
		// writes the client's packet, and reads the server's, from inside its drawing of a frame as well:
		// a recording's first messages given there (when going back) restart the client game under the
		// drawing's feet (the crashes at 0x1401AFEEA and 0x14013C376).
		run_commands();
		play_tick();
		if (g_play == Play::Playing)
		{
			// The client keeps a count of the commands it has for the server that the server has not
			// taken yet, and gives the match up at 128 ("EXE_DISCONNECTED"). A recording takes them all;
			// that was only said when a message was given, so with time frozen, or at the recording's
			// end, the count ran up (every setting changed in the Look tab is such a command).
			*reinterpret_cast<std::int32_t*>(CLC_RELIABLE_ACKNOWLEDGE) = *reinterpret_cast<std::int32_t*>(CLC_RELIABLE_SEQUENCE);
		}

		if (g_play != Play::Playing)
		{
			return g_original_time_scale_msec(msec);
		}

		// a jump made with time frozen: played up to its time
		if (const std::int64_t roll = g_roll_to; roll >= 0)
		{
			const std::uint64_t clock = g_clock;
			if (clock >= static_cast<std::uint64_t>(roll))
			{
				g_roll_to = -1;
			}
			else if (!g_caught_up || msec <= 0)
			{
				pin_client_time(0);
				return 0;
			}
			else
			{
				int step = static_cast<int>(static_cast<float>(msec > 50 ? 50 : msec) * ROLL_SPEED);
				if (clock + static_cast<std::uint64_t>(step) >= static_cast<std::uint64_t>(roll))
				{
					step = static_cast<int>(static_cast<std::uint64_t>(roll) - clock);
					g_roll_to = -1;
				}

				g_clock = clock + static_cast<std::uint64_t>(step);
				g_clock_fraction = 0.0f;
				pin_client_time(step);
				return step;
			}
		}

		if (g_paused || msec <= 0)
		{
			pin_client_time(0);
			return 0;
		}

		const float capture = g_capture_fps;
		g_clock_fraction += (capture > 0.0f ? 1000.0f / capture : static_cast<float>(msec > 200 ? 200 : msec)) * SPEEDS[g_speed];
		const int whole = static_cast<int>(g_clock_fraction);
		g_clock_fraction -= static_cast<float>(whole);
		g_clock += static_cast<std::uint64_t>(whole);
		pin_client_time(whole);
		return whole;
	}

	using save_predicted_t = void (*)(unsigned char*, int);
	save_predicted_t g_original_save_predicted = nullptr;
	using get_predicted_t = bool (*)(unsigned char*, int, unsigned char*);
	get_predicted_t g_original_get_predicted = nullptr;
	using draw_active_frame_t = int (*)(int, int, int, int, int, int, unsigned);
	draw_active_frame_t g_original_draw_active_frame = nullptr;

	// Recording: each position the client keeps for a command goes into the match's file too.
	void hooked_save_predicted(unsigned char* cl, const int server_time)
	{
		g_original_save_predicted(cl, server_time);
		if (g_play != Play::Idle || !cl)
		{
			return;
		}

		EnterCriticalSection(&g_lock);
		static std::int32_t last_time = 0;
		if (g_file && server_time != last_time)
		{
			last_time = server_time;
			Predicted predicted;
			predicted.serverTime = server_time;
			memcpy(predicted.origin, cl + CL_ORIGIN, sizeof(predicted.origin));
			memcpy(predicted.velocity, cl + CL_VELOCITY, sizeof(predicted.velocity));
			memcpy(&predicted.bobCycle, cl + CL_BOB_CYCLE, sizeof(predicted.bobCycle));
			memcpy(&predicted.movementDir, cl + CL_MOVEMENT_DIR, sizeof(predicted.movementDir));
			const std::uint32_t head[3] = {static_cast<std::uint32_t>(GetTickCount64() - g_file_start), KIND_PREDICTED,
			                               static_cast<std::uint32_t>(sizeof(predicted) + CL_BLOCK_SIZE)};
			fwrite(head, sizeof(head), 1, g_file);
			fwrite(&predicted, sizeof(predicted), 1, g_file);
			fwrite(cl + CL_BLOCK, CL_BLOCK_SIZE, 1, g_file);
		}

		LeaveCriticalSection(&g_lock);
	}

	// Playback: the recorded player's position of a time, where the game asks for what it predicted.
	bool hooked_get_predicted(unsigned char* cl, const int server_time, unsigned char* ps)
	{
		const bool found = g_original_get_predicted(cl, server_time, ps);
		if (g_play == Play::Idle && ps)
		{
			EnterCriticalSection(&g_lock);
			static ULONGLONG last = 0;
			const ULONGLONG now = GetTickCount64();
			if (g_file && now - last >= 1000)
			{
				last = now;
				const std::uint32_t head[3] = {static_cast<std::uint32_t>(now - g_file_start), KIND_PLAYER_STATE,
				                               static_cast<std::uint32_t>(PLAYER_STATE_SIZE)};
				fwrite(head, sizeof(head), 1, g_file);
				fwrite(ps, PLAYER_STATE_SIZE, 1, g_file);
			}

			LeaveCriticalSection(&g_lock);
		}

		if (g_play != Play::Playing || !ps || g_predicted.empty())
		{
			return found;
		}

		// the last one at or before the time
		size_t low = 0, high = g_predicted.size();
		while (low < high)
		{
			const size_t middle = (low + high) / 2;
			if (g_predicted[middle].serverTime <= server_time)
			{
				low = middle + 1;
			}
			else
			{
				high = middle;
			}
		}

		if (low == 0 || server_time - g_predicted[low - 1].serverTime > 500)
		{
			++g_predicted_missed;
			return found;
		}

		PredictedView predicted = g_predicted[low - 1];
		if (low < g_predicted.size())
		{
			// between this record and the next, to the part of a millisecond: slow motion shows the steps
			const PredictedView& next = g_predicted[low];
			const int span = next.serverTime - predicted.serverTime;
			float apart = 0.0f;
			for (int i = 0; i < 3; ++i)
			{
				apart += (next.origin[i] - predicted.origin[i]) * (next.origin[i] - predicted.origin[i]);
			}

			if (span > 0 && span <= 100 && apart < 150.0f * 150.0f)
			{
				float t = (static_cast<float>(server_time - predicted.serverTime) + g_clock_fraction) / static_cast<float>(span);
				t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
				for (int i = 0; i < 3; ++i)
				{
					predicted.origin[i] += (next.origin[i] - predicted.origin[i]) * t;
					if (predicted.hasView && next.hasView)
					{
						float turn = next.viewangles[i] - predicted.viewangles[i];
						turn -= 360.0f * floorf(turn / 360.0f + 0.5f);
						predicted.viewangles[i] += turn * t;
					}
				}
			}
		}

		memcpy(ps + PS_ORIGIN, predicted.origin, sizeof(predicted.origin));
		memcpy(ps + PS_VELOCITY, predicted.velocity, sizeof(predicted.velocity));
		ps[PS_BOB_CYCLE] = static_cast<unsigned char>(predicted.bobCycle);
		memcpy(ps + PS_MOVEMENT_DIR, &predicted.movementDir, sizeof(predicted.movementDir));
		if (predicted.hasView)
		{
			float delta[3], view[3];
			memcpy(delta, ps + PS_DELTA_ANGLES, sizeof(delta));
			for (int i = 0; i < 3; ++i)
			{
				view[i] = predicted.viewangles[i] + delta[i];
			}

			memcpy(ps + PS_VIEWANGLES, view, sizeof(view));
		}

		++g_predicted_given;
		return true;
	}

	int hooked_draw_active_frame(const int client, const int server_time, int demo_type, const int cubemap_shot,
	                             const int cubemap_size, const int render_screen, const unsigned more)
	{
		if (g_play == Play::Playing && g_demo_mode)
		{
			demo_type = DEMO_TYPE_CLIENT;
		}

		return g_original_draw_active_frame(client, server_time, demo_type, cubemap_shot, cubemap_size, render_screen, more);
	}

	// The menus are told when a value the server sets for them (an "omnvar") changes. The match's own
	// "choose a team" and "choose a class" menus are opened that way (ui_options_menu), and a recording
	// has it from the match's start; someone watching has nothing to choose.
	constexpr uintptr_t LUI_OMNVAR_UPDATE = 0x1400D9680;
	using omnvar_update_t = void (*)(int, const char**, void*, void*);
	omnvar_update_t g_original_omnvar_update = nullptr;

	// The same value is also read by the menus themselves when they are built (the first time a
	// recording plays), so it is cleared where they read it from as well: in each snapshot the client
	// game takes, before the game compares it with the last one (0x1401C41B0), and in the game's own copy.
	// Omnvars: 0x40 bytes each from 0x14B503900, the name first; the per-client ones come first (their
	// count at 0x14B508B88) and are 8 bytes each from +0x5118 of a client game snapshot, the game's ones
	// follow and are from +0x4C44.
	constexpr uintptr_t CG_OMNVARS_CHANGED = 0x1401C41B0, OMNVAR_DEFS = 0x14B503900, OMNVAR_CLIENT_COUNT = 0x14B508B88,
		OMNVAR_GAME_COUNT = 0x14B508B84, OMNVAR_STORED = 0x1404C7010;
	constexpr size_t SNAP_CLIENT_OMNVARS = 0x5118, SNAP_GAME_OMNVARS = 0x4C44;
	using omnvars_changed_t = void (*)(int, std::uint8_t*, std::uint8_t*);
	omnvars_changed_t g_original_omnvars_changed = nullptr;
	std::atomic<int> g_options_menu_index{-2};

	int options_menu_index()
	{
		int index = g_options_menu_index;
		if (index == -2)
		{
			index = -1;
			const unsigned count = *reinterpret_cast<unsigned*>(OMNVAR_CLIENT_COUNT) + *reinterpret_cast<unsigned*>(OMNVAR_GAME_COUNT);
			for (unsigned i = 0; i < count && i < 4096; ++i)
			{
				const char* name = *reinterpret_cast<const char**>(OMNVAR_DEFS + i * 0x40ull);
				if (name && !IsBadReadPtr(name, 16) && strcmp(name, "ui_options_menu") == 0)
				{
					index = static_cast<int>(i);
					break;
				}
			}

			g_options_menu_index = index;
			log_line("menus: ui_options_menu is omnvar %d of %u (%u per client)", index, count, *reinterpret_cast<unsigned*>(OMNVAR_CLIENT_COUNT));
		}

		return index;
	}

	void clear_options_menu(std::uint8_t* snapshot)
	{
		const int index = options_menu_index();
		const int per_client = static_cast<int>(*reinterpret_cast<unsigned*>(OMNVAR_CLIENT_COUNT));
		if (index < 0 || !snapshot)
		{
			return;
		}

		std::uint8_t* value = index < per_client ? snapshot + SNAP_CLIENT_OMNVARS + index * 8ull
		                                         : snapshot + SNAP_GAME_OMNVARS + (index - per_client) * 8ull;
		memset(value, 0, 8);
	}

	void hooked_omnvars_changed(const int client, std::uint8_t* before, std::uint8_t* now)
	{
		if (g_play != Play::Idle)
		{
			clear_options_menu(before);
			clear_options_menu(now);
			const int index = options_menu_index();
			if (index >= 0)
			{
				if (void* stored = reinterpret_cast<void* (*)(int, int)>(OMNVAR_STORED)(client, index))
				{
					memset(stored, 0, 8);
				}
			}
		}

		g_original_omnvars_changed(client, before, now);
	}

	void hooked_omnvar_update(const int client, const char** def, void* value, void* state)
	{
		if (g_play != Play::Idle && def && *def && strcmp(*def, "ui_options_menu") == 0)
		{
			++g_menus_kept_shut;
			return;
		}

		// which of the server's values for the menus and HUD change in a recording, and when: each name
		// is logged the first time it changes in a playback (the HUD's parts and the screen effects of
		// a dash are driven by these)
		if (g_play == Play::Playing && def && *def)
		{
			static std::mutex lock;
			static std::vector<std::string> seen;
			static std::uint64_t seen_for = ~0ull;
			std::lock_guard<std::mutex> guard(lock);
			if (seen_for != g_playback_number)
			{
				seen_for = g_playback_number;
				seen.clear();
			}

			bool known = false;
			for (const std::string& name : seen)
			{
				known = known || name == *def;
			}

			if (!known && seen.size() < 400)
			{
				seen.emplace_back(*def);
				log_line("omnvar: %s first changes at %.2f s (type %d)", *def, g_clock.load() / 1000.0, reinterpret_cast<const int*>(def)[3]);
			}
		}

		g_original_omnvar_update(client, def, value, state);
	}

	// The minimap, which a recording's viewer has no use for and which does not come out right in
	// playback (no map picture, its rings over the whole screen): its two drawing functions in the
	// menus' code do nothing while a recording plays.
	constexpr uintptr_t LUI_MINIMAP_DRAW = 0x14011C570, LUI_MINIMAP_DRAW_2 = 0x14011C930;
	using minimap_draw_t = void (*)(int, void*, void*, void*);
	minimap_draw_t g_original_minimap_draw = nullptr, g_original_minimap_draw_2 = nullptr;

	void hooked_minimap_draw(const int client, void* a, void* b, void* c)
	{
		if (g_play == Play::Idle)
		{
			g_original_minimap_draw(client, a, b, c);
		}
	}

	void hooked_minimap_draw_2(const int client, void* a, void* b, void* c)
	{
		if (g_play == Play::Idle)
		{
			g_original_minimap_draw_2(client, a, b, c);
		}
	}

	// The game's errors (Com_Error, s1-mod's symbols), written to the log with their text: an error
	// that ends a match says why only on a screen that may never be seen.
	constexpr uintptr_t COM_ERROR = 0x1403CE480;
	using com_error_t = void (*)(int, const char*, ...);
	com_error_t g_original_com_error = nullptr;

	void hooked_com_error(const int code, const char* format, ...)
	{
		char text[1024] = {};
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text) - 1, format ? format : "", args);
		va_end(args);
		sky_put_back_now();
		// with where it was raised from: the same text is raised from many places
		char from[512] = "";
		size_t length = 0;
		const auto* stack = static_cast<const std::uint64_t*>(_AddressOfReturnAddress());
		for (int i = 0, found = 0; i < 300 && found < 14; ++i)
		{
			std::uint64_t value = 0;
			if (!read_bytes(reinterpret_cast<std::uint64_t>(stack + i), &value, sizeof(value)))
			{
				break;
			}

			if (value >= 0x140001000ull && value < 0x1407E9000ull)
			{
				length += sprintf_s(from + length, sizeof(from) - length, " %llX", value);
				++found;
			}
		}

		log_line("GAME ERROR (kind %d): %s; playback %d at %.2f s%s, connection state %d; from:%s", code, text, static_cast<int>(g_play.load()),
		         g_clock.load() / 1000.0, g_paused ? " (paused)" : "", connection_state(), from);
		g_original_com_error(code, "%s", text);
	}

	// The game's clock is in whole milliseconds, so in slow motion the world moved a step every few
	// frames (below about 0.1x it showed). Playback keeps the part of a millisecond left over; it is
	// added to how far between its two snapshots the client game takes itself to be, which is what
	// everything in the world is placed by.
	using set_frame_interpolation_t = void (*)(int);
	set_frame_interpolation_t g_original_set_frame_interpolation = nullptr;

	void hooked_set_frame_interpolation(const int client)
	{
		g_original_set_frame_interpolation(client);
		if (g_play != Play::Playing)
		{
			return;
		}

		const auto* snap = *reinterpret_cast<const unsigned char**>(CG_SNAP);
		const auto* next = *reinterpret_cast<const unsigned char**>(CG_NEXT_SNAP);
		if (!snap || !next)
		{
			return;
		}

		const int span = *reinterpret_cast<const int*>(next + CG_SNAPSHOT_SERVER_TIME) - *reinterpret_cast<const int*>(snap + CG_SNAPSHOT_SERVER_TIME);
		if (span > 0)
		{
			*reinterpret_cast<float*>(CG_FRAME_INTERPOLATION) += g_clock_fraction / static_cast<float>(span);
		}
	}

	// Every setting the game has, written out once a session (theater\dvars.txt): what the Look tab
	// can be built from.
	void dump_dvars()
	{
		static bool done = false;
		if (done)
		{
			return;
		}

		done = true;
		char path[MAX_PATH];
		sprintf_s(path, "%s\\dvars.txt", g_folder);
		FILE* file = nullptr;
		if (fopen_s(&file, path, "w") || !file)
		{
			return;
		}

		const int count = theater_dvar_count();
		fprintf(file, "%d dvars: name | type | flags | current | reset | limits\n", count);
		for (int i = 0; i < count; ++i)
		{
			const auto* dvar = static_cast<const unsigned char*>(theater_dvar_at(i));
			if (!dvar || IsBadReadPtr(dvar, 0x50))
			{
				continue;
			}

			const char* name = *reinterpret_cast<const char* const*>(dvar);
			if (!name || IsBadReadPtr(name, 4))
			{
				continue;
			}

			const int type = static_cast<signed char>(dvar[0xC]);
			const unsigned flags = *reinterpret_cast<const unsigned*>(dvar + 8);
			fprintf(file, "%s | %d | %X | ", name, type, flags);
			for (const size_t at : {size_t{0x10}, size_t{0x30}})
			{
				const auto* value = dvar + at;
				const float* f = reinterpret_cast<const float*>(value);
				switch (type)
				{
				case 0: fprintf(file, "%d", value[0] ? 1 : 0); break;
				case 1: fprintf(file, "%g", f[0]); break;
				case 2: fprintf(file, "%g %g", f[0], f[1]); break;
				case 3: fprintf(file, "%g %g %g", f[0], f[1], f[2]); break;
				case 4: fprintf(file, "%g %g %g %g", f[0], f[1], f[2], f[3]); break;
				case 5:
				case 6: fprintf(file, "%d", *reinterpret_cast<const int*>(value)); break;
				case 7:
				{
					const char* text = *reinterpret_cast<const char* const*>(value);
					fprintf(file, "\"%.200s\"", text && !IsBadReadPtr(text, 1) ? text : "");
					break;
				}
				case 8:
				case 9: fprintf(file, "%u %u %u %u", value[0], value[1], value[2], value[3]); break;
				default: fprintf(file, "?"); break;
				}

				fprintf(file, " | ");
			}

			const auto* limits = dvar + 0x40;
			if (type == 5)
			{
				fprintf(file, "%d..%d", *reinterpret_cast<const int*>(limits), *reinterpret_cast<const int*>(limits + 4));
			}
			else if (type >= 1 && type <= 4)
			{
				fprintf(file, "%g..%g", *reinterpret_cast<const float*>(limits), *reinterpret_cast<const float*>(limits + 4));
			}
			else if (type == 6)
			{
				const int names = *reinterpret_cast<const int*>(limits);
				const char* const* list = *reinterpret_cast<const char* const* const*>(limits + 8);
				for (int n = 0; n < names && n < 40 && list && !IsBadReadPtr(list + n, 8); ++n)
				{
					fprintf(file, "%s%s", n ? "," : "", list[n] && !IsBadReadPtr(list[n], 1) ? list[n] : "?");
				}
			}

			fprintf(file, "\n");
		}

		fclose(file);
		log_line("the game's %d settings are listed in theater\\dvars.txt", count);
	}

	// ---- the HUD's clock. The game's HUD and menus (LUI) are stepped by 0x1400D7480(client, root, ms, ms,
	// flag, lua): its fourth argument is the real time since the root's last step (0x1400E5790 takes it
	// from the system's clock, kept at +0xA8 of the root's block) and goes to the scripts as the step's
	// "deltaTime": every timer and animation runs on it (the kill feed's lines fading, the score popups,
	// the hit marker). In playback it is given the recording's clock instead, so the HUD stands still with
	// a paused recording, runs slow in slow motion, and is stepped a frame's worth when a video is taken.
	// Not while one of the game's menus has the keys (it would stand still with the recording), and not
	// for a moment after the editor has told the HUD's script something (the script runs on a timer).
	constexpr std::uintptr_t LUI_STEP = 0x1400D7480, KEY_CATCHERS = 0x1417E168C;
	using lui_step_t = void (*)(int, void*, int, int, int, void*);
	lui_step_t g_original_lui_step = nullptr;
	std::atomic<ULONGLONG> g_hud_real_until{0};

	void hooked_lui_step(const int client, void* root, const int other, const int real, const int flag, void* lua)
	{
		struct Root
		{
			void* root;
			std::uint64_t clock;
		};
		static Root roots[6] = {};
		static int told = 0;
		int given = real;
		if (g_play == Play::Playing)
		{
			Root* mine = nullptr;
			for (Root& one : roots)
			{
				if (one.root == root || !one.root)
				{
					mine = &one;
					break;
				}
			}

			if (mine)
			{
				const std::uint64_t clock = g_clock;
				const int catchers = *reinterpret_cast<const int*>(KEY_CATCHERS);
				const bool known = mine->root == root;
				const bool follows = known && (catchers & ~1) == 0 && GetTickCount64() >= g_hud_real_until;
				if (follows)
				{
					const std::uint64_t passed = clock > mine->clock ? clock - mine->clock : 0;
					given = static_cast<int>(passed > 60000 ? 60000 : passed);
				}

				if (told < 6 && (!known || (!follows && (catchers & ~1) != 0)))
				{
					++told;
					log_line("hud clock: root %p %s (key catchers %X)", root, follows || !known ? "follows the recording's clock" : "is left on real time", catchers);
				}

				mine->root = root;
				mine->clock = clock;
			}
		}
		else if (roots[0].root)
		{
			memset(roots, 0, sizeof(roots));
		}

		g_original_lui_step(client, root, other, given, flag, lua);
	}

	bool hook(const uintptr_t address, void* detour, void* original, const char* name)
	{
		MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(address), detour, static_cast<LPVOID*>(original));
		if (status == MH_OK)
		{
			status = MH_EnableHook(reinterpret_cast<LPVOID>(address));
		}

		if (status != MH_OK)
		{
			log_line("hook: %s FAILED (%s)", name, MH_StatusToString(status));
		}

		return status == MH_OK;
	}

	// A fault in the game's own code, noted with where it came from: the game's crash reports have not
	// had the stack. (First-chance: faults the game handles itself are noted too, so only a few.)
	LONG CALLBACK note_fault(EXCEPTION_POINTERS* info)
	{
		const auto* record = info->ExceptionRecord;
		const auto* context = info->ContextRecord;
		if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || context->Rip < 0x140001000ull || context->Rip >= 0x1407E9000ull ||
		    g_faults_noted.fetch_add(1) >= 4)
		{
			return EXCEPTION_CONTINUE_SEARCH;
		}

		log_line("FAULT at %llX %s %llX; play state %d, connection state %d; rax %llX rbx %llX rcx %llX rdx %llX rsi %llX rdi %llX r8 %llX",
		         context->Rip, record->ExceptionInformation[0] ? "writing" : "reading", record->ExceptionInformation[1],
		         static_cast<int>(g_play.load()), connection_state(), context->Rax, context->Rbx, context->Rcx, context->Rdx,
		         context->Rsi, context->Rdi, context->R8);
		char line[1024] = "  called from:";
		size_t length = strlen(line);
		int found = 0;
		for (int i = 0; i < 400 && found < 24; ++i)
		{
			std::uint64_t value = 0;
			if (!read_bytes(context->Rsp + i * 8ull, &value, sizeof(value)))
			{
				break;
			}

			if (value >= 0x140001000ull && value < 0x1407E9000ull)
			{
				length += sprintf_s(line + length, sizeof(line) - length, " %llX", value);
				++found;
			}
		}

		log_line("%s", line);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	bool game_has_focus()
	{
		DWORD process = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &process);
		return process == GetCurrentProcessId();
	}

	// F9 plays the newest recording, F10 stops. At the menu the client neither has messages nor sends
	// packets, so a start asked for there is made from this thread (the command buffer is the game's to
	// run, on its own thread).
	DWORD WINAPI keys(LPVOID)
	{
		bool f9 = false, f10 = false, f8 = false;
		for (;;)
		{
			Sleep(30);
			const bool focus = game_has_focus();
			const bool f9_now = focus && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
			const bool f10_now = focus && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
			if (f9_now && !f9)
			{
				log_line("F9");
				g_play_request = true;
			}

			if (f10_now && !f10)
			{
				log_line("F10");
				g_stop_request = true;
			}

			static bool down = false, left = false, right = false, up = false, page_down = false, home = false;
			const bool playing = focus && g_play == Play::Playing && !theater_keys_blocked();
			const bool down_now = playing && (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;      // play and pause
			const bool left_now = playing && (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;       // slower
			const bool right_now = playing && (GetAsyncKeyState(VK_UP) & 0x8000) != 0;        // faster
			const bool up_now = playing && (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;        // 10 s on
			if (down_now && !down)
			{
				g_pause_request = true;
			}

			if (left_now && !left)
			{
				g_slower_request = true;
			}

			if (right_now && !right)
			{
				g_faster_request = true;
			}

			if (up_now && !up)
			{
				g_skip_request = true;
			}

			const bool page_down_now = playing && (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;  // 10 s back
			const bool home_now = false;  // (Home went back to the start; not wanted)
			if (page_down_now && !page_down)
			{
				g_back_request = true;
			}

			if (home_now && !home)
			{
				g_restart_request = true;
			}

			page_down = page_down_now;
			home = home_now;
			down = down_now;
			left = left_now;
			right = right_now;
			up = up_now;
			const bool f8_now = focus && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
			if (f8_now && !f8 && g_play == Play::Playing)
			{
				g_demo_mode = !g_demo_mode;
				log_line("F8: demo mode %s", g_demo_mode ? "on" : "off");
			}

			f8 = f8_now;
			f9 = f9_now;
			f10 = f10_now;
			// nothing of the main thread's runs here at the menu or while a map loads: a request it has not
			// taken in 300 ms is taken here, and a start is watched from here
			static ULONGLONG waiting_since = 0;
			if (g_play_request || g_stop_request)
			{
				const ULONGLONG now = GetTickCount64();
				if (!waiting_since)
				{
					waiting_since = now;
				}
				else if (now - waiting_since > 300)
				{
					waiting_since = 0;
					play_tick(false);
				}
			}
			else
			{
				waiting_since = 0;
				if (g_play == Play::Starting)
				{
					play_tick(false);
				}
			}
		}
	}

	// The killcam crash (0xC0000005 at 0x14044F762, when a bot's kill is shown in a private match): the
	// function that reads or writes one entity's change (0x1403D76F0) begins with a jump into the game's
	// protection code, which there does `xor ebx, a` then `xor ebx, b` (a == b: no change to ebx, but a
	// 32-bit operation clears the upper half of rbx) before going on at 0x15121E509. Its caller keeps its
	// message in rbx, a pointer above 4 GB when its stack is, and crashes on it afterwards. The fix (found by
	// bradstv for CBServers' s1-mod) jumps past the pair.
	constexpr uintptr_t DELTA_ENTITY = 0x1403D76F0;
	constexpr uintptr_t DELTA_ENTITY_JUNK = 0x15121E4FD;
	constexpr uintptr_t DELTA_ENTITY_AFTER_JUNK = 0x15121E509;

	void fix_killcam()
	{
		unsigned char now[5] = {};
		std::int32_t to = 0;
		if (!read_bytes(DELTA_ENTITY, now, sizeof(now)))
		{
			log_line("killcam fix: the function could not be read; not applied");
			return;
		}

		memcpy(&to, now + 1, sizeof(to));
		const uintptr_t target = DELTA_ENTITY + 5 + static_cast<std::intptr_t>(to);
		if (now[0] == 0xE9 && target == DELTA_ENTITY_AFTER_JUNK)
		{
			log_line("killcam fix: already in place (another mod's)");
			return;
		}

		unsigned char junk[12] = {};
		if (now[0] != 0xE9 || target != DELTA_ENTITY_JUNK || !read_bytes(DELTA_ENTITY_JUNK, junk, sizeof(junk)) ||
		    junk[0] != 0x33 || junk[1] != 0x1D || junk[6] != 0x33 || junk[7] != 0x1D)
		{
			log_line("killcam fix: the game's code is not as expected there (%02X, to %llX); not applied", now[0],
			         static_cast<unsigned long long>(target));
			return;
		}

		const std::int32_t fixed = static_cast<std::int32_t>(DELTA_ENTITY_AFTER_JUNK - (DELTA_ENTITY + 5));
		DWORD old = 0;
		if (!VirtualProtect(reinterpret_cast<LPVOID>(DELTA_ENTITY), 5, PAGE_EXECUTE_READWRITE, &old))
		{
			log_line("killcam fix: the code could not be made writable; not applied");
			return;
		}

		// the four bytes of the jump's distance at once
		InterlockedExchange(reinterpret_cast<volatile LONG*>(DELTA_ENTITY + 1), fixed);
		VirtualProtect(reinterpret_cast<LPVOID>(DELTA_ENTITY), 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(DELTA_ENTITY), 5);
		log_line("killcam fix: applied");
	}

	// MinHook needs free memory within 1 GB of the function it patches, and a running game can have
	// taken it all: a few spots are kept from the start and given back right before hooking. (Other
	// addresses than the debug overlay's, which does the same.)
	void* g_reserved[32];
	int g_reserved_count = 0;

	void reserve_hook_memory()
	{
		for (uintptr_t address = 0x13F200000ull; address > 0x120000000ull && g_reserved_count < 16; address -= 0x400000ull)
		{
			if (void* block = VirtualAlloc(reinterpret_cast<void*>(address), 0x10000, MEM_RESERVE, PAGE_NOACCESS))
			{
				g_reserved[g_reserved_count++] = block;
			}
		}

		for (uintptr_t address = 0x152200000ull; address < 0x170000000ull && g_reserved_count < 32; address += 0x400000ull)
		{
			if (void* block = VirtualAlloc(reinterpret_cast<void*>(address), 0x10000, MEM_RESERVE, PAGE_NOACCESS))
			{
				g_reserved[g_reserved_count++] = block;
			}
		}
	}

	void release_hook_memory()
	{
		for (int i = 0; i < g_reserved_count; ++i)
		{
			VirtualFree(g_reserved[i], 0, MEM_RELEASE);
		}

		g_reserved_count = 0;
	}

	// The program's name as Windows has it now, in lower case. S1x starts as s1x.exe and names the game's
	// program only once it has loaded it, which can be after this DLL is loaded.
	void get_program(char* exe)
	{
		exe[0] = 0;
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		_strlwr_s(exe, MAX_PATH);
	}

	DWORD WINAPI worker(LPVOID)
	{
		// which game this is, once S1x has said
		char exe[MAX_PATH];
		get_program(exe);
		log_line("program at load: %s", exe);
		for (int i = 0; i < 600 && !strstr(exe, "s1_mp64_ship") && !strstr(exe, "s1_sp64_ship"); ++i)
		{
			Sleep(50);
			get_program(exe);
		}

		log_line("program now: %s", exe);
		if (strstr(exe, "s1_sp64_ship"))
		{
			log_line("single player: nothing to do");
			release_hook_memory();
			return 0;
		}

		// the game unpacks its code as it starts: wait for the function to be there (whatever the
		// program is called: the function's bytes are the multiplayer game's alone)
		bool unpacked = false;
		for (int i = 0; i < 2400 && !unpacked; ++i)
		{
			unsigned char now[sizeof(PROLOGUE)];
			unpacked = read_bytes(CL_PARSE_SERVER_MESSAGE, now, sizeof(now)) && memcmp(now, PROLOGUE, sizeof(now)) == 0;
			if (!unpacked)
			{
				Sleep(50);
			}
		}

		if (!unpacked)
		{
			unsigned char now[16] = {};
			read_bytes(CL_PARSE_SERVER_MESSAGE, now, sizeof(now));
			log_line("the game's message function never looked as expected (%02X %02X %02X %02X %02X %02X %02X %02X): "
			         "a different game build? Nothing was hooked.",
			         now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7]);
			release_hook_memory();
			return 0;
		}

		// 0.2 hooked the moment the code was unpacked, and the game crashed while starting: its start
		// is left alone now (as the debug overlay, which hooks later, leaves it), and the function is
		// only patched once it has stayed the same for a while
		log_line("the game's code is unpacked; waiting %d s for the game to finish starting", HOOK_DELAY_S);
		for (int i = 0; i < HOOK_DELAY_S; ++i)
		{
			Sleep(1000);
		}

		unsigned char now[sizeof(PROLOGUE)];
		if (!read_bytes(CL_PARSE_SERVER_MESSAGE, now, sizeof(now)) || memcmp(now, PROLOGUE, sizeof(now)) != 0)
		{
			log_line("the game's message function changed while the game started: nothing was hooked");
			release_hook_memory();
			return 0;
		}

		fix_killcam();
		release_hook_memory();
		MH_STATUS status = MH_Initialize();
		if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
		{
			log_line("hooking could not start (%s)", MH_StatusToString(status));
			return 0;
		}

		status = MH_CreateHook(reinterpret_cast<LPVOID>(CL_PARSE_SERVER_MESSAGE), reinterpret_cast<LPVOID>(&hooked_parse),
		                       reinterpret_cast<LPVOID*>(&g_original_parse));
		if (status == MH_OK)
		{
			status = MH_EnableHook(reinterpret_cast<LPVOID>(CL_PARSE_SERVER_MESSAGE));
		}

		if (status == MH_OK)
		{
			MH_STATUS write = MH_CreateHook(reinterpret_cast<LPVOID>(CL_WRITE_PACKET), reinterpret_cast<LPVOID>(&hooked_write_packet),
			                                reinterpret_cast<LPVOID*>(&g_original_write_packet));
			if (write == MH_OK)
			{
				write = MH_EnableHook(reinterpret_cast<LPVOID>(CL_WRITE_PACKET));
			}

			log_line(write == MH_OK ? "hook: client packets installed. F9 at the menu plays the newest recording, F10 stops."
			                        : "hook: client packets FAILED (%s); no playback",
			         MH_StatusToString(write));
			if (write == MH_OK)
			{
				const bool server = hook(SV_FRAME, &hooked_sv_frame, &g_original_sv_frame, "the local server's step");
				const bool bots = hook(SV_ADD_BOT, &hooked_add_bot, &g_original_add_bot, "bots") &&
					hook(SV_ADD_TEST_CLIENT, &hooked_add_test_client, &g_original_add_test_client, "test clients");
				log_line("hook: playback's local match: server step %s, bots %s", server ? "held" : "NOT held",
				         bots ? "kept out" : "NOT kept out");
				const bool time = hook(COM_TIME_SCALE_MSEC, &hooked_time_scale_msec, &g_original_time_scale_msec, "the game's time");
				log_line("hook: playback's pause and speed %s (Space pauses, up and down arrows change speed, right skips 10 s, left goes back 10 s)",
				         time ? "yes" : "NO");
				const bool save = hook(CL_SAVE_PREDICTED, &hooked_save_predicted, &g_original_save_predicted, "the player's position, kept");
				const bool get = hook(CL_GET_PREDICTED, &hooked_get_predicted, &g_original_get_predicted, "the player's position, asked for");
				const bool draw = hook(CG_DRAW_ACTIVE_FRAME, &hooked_draw_active_frame, &g_original_draw_active_frame, "the frame");
				log_line("hook: the player's own view: recorded %s, played back %s, demo mode %s (F8 switches it while playing)",
				         save ? "yes" : "NO", get ? "yes" : "NO", draw ? "yes" : "NO");
				const bool hud_clock = hook(LUI_STEP, &hooked_lui_step, &g_original_lui_step, "the HUD's clock");
				log_line("hook: the HUD's clock follows the recording %s", hud_clock ? "yes" : "NO");
				const bool menus = hook(LUI_OMNVAR_UPDATE, &hooked_omnvar_update, &g_original_omnvar_update, "the team and class menus");
				const bool minimap = hook(LUI_MINIMAP_DRAW, &hooked_minimap_draw, &g_original_minimap_draw, "the minimap") &&
					hook(LUI_MINIMAP_DRAW_2, &hooked_minimap_draw_2, &g_original_minimap_draw_2, "the minimap's second part");
				log_line("hook: playback's minimap %s", minimap ? "hidden" : "NOT hidden");
				const bool values = hook(CG_OMNVARS_CHANGED, &hooked_omnvars_changed, &g_original_omnvars_changed, "the menus' values");
				log_line("hook: playback's team and class menus %s, their value %s", menus ? "kept shut" : "NOT kept shut",
				         values ? "cleared" : "NOT cleared");
				const bool smooth = hook(CG_SET_FRAME_INTERPOLATION, &hooked_set_frame_interpolation, &g_original_set_frame_interpolation,
				                         "the time between snapshots");
				log_line("hook: slow motion %s", smooth ? "is smooth below a millisecond a frame" : "is NOT smoothed");
				const bool errors = hook(COM_ERROR, &hooked_com_error, &g_original_com_error, "the game's errors");
				log_line("hook: the game's errors %s", errors ? "are written here" : "are NOT written here");
				ui_start();
				camera_start();
				sun_start();
				models_start();
				passes_start();
				effects_start();
				if (HANDLE thread = CreateThread(nullptr, 0, &keys, nullptr, 0, nullptr))
				{
					CloseHandle(thread);
				}
			}
		}

		AddVectoredExceptionHandler(1, &note_fault);
		log_line(status == MH_OK ? "hook: server messages installed. Play a private match; it is recorded by itself."
		                         : "hook: server messages FAILED (%s)",
		         MH_StatusToString(status));
		// how long the game lives with the hook in, for the log
		for (int i = 1; i <= 12 && status == MH_OK; ++i)
		{
			Sleep(5000);
			log_line("still running %d s after the hook (%llu calls so far)", i * 5, g_calls.load());
		}

		return 0;
	}
}

TheaterState theater_state()
{
	TheaterState state;
	state.playing = g_play == Play::Playing;
	state.starting = g_play == Play::Starting;
	state.paused = g_paused;
	state.speed = SPEEDS[g_speed];
	const std::int64_t roll = g_roll_to;
	state.position_ms = static_cast<std::uint32_t>(roll >= 0 ? static_cast<std::uint64_t>(roll) : g_clock.load());
	state.length_ms = g_length_ms;
	state.position_exact = roll >= 0 ? static_cast<double>(roll) : static_cast<double>(g_clock.load()) + g_clock_fraction;
	state.settled = g_settled && roll < 0;
	state.rewinds = g_rewinds;
	state.capture_fps = g_capture_fps;
	strncpy_s(state.name, g_shown_name, _TRUNCATE);
	strncpy_s(state.map, g_shown_map, _TRUNCATE);
	return state;
}

void theater_pause_toggle() { g_pause_request = true; }
void theater_slower() { g_slower_request = true; }
void theater_faster() { g_faster_request = true; }
void theater_seek(const std::uint32_t ms) { g_seek_request = ms; }
void theater_rebuild() { g_rebuild_request = true; }

void theater_set_speed(const float speed)
{
	int best = 0;
	for (int i = 0; i < static_cast<int>(sizeof(SPEEDS) / sizeof(SPEEDS[0])); ++i)
	{
		if (fabsf(SPEEDS[i] - speed) < fabsf(SPEEDS[best] - speed))
		{
			best = i;
		}
	}

	g_speed = best;
}

int theater_speed_steps(const float** steps)
{
	*steps = SPEEDS;
	return static_cast<int>(sizeof(SPEEDS) / sizeof(SPEEDS[0]));
}
void theater_stop() { g_stop_request = true; }
void theater_capture(const float fps) { g_capture_fps = fps; }

void theater_set_paused(const bool paused)
{
	if (g_paused != paused)
	{
		g_pause_request = true;
	}
}

namespace
{
	constexpr std::uintptr_t DB_ENUM_XASSETS = 0x14026EC80;  // s1-mod: DB_EnumXAssets_Internal
	constexpr int ASSET_GFX_WORLD = 31;

	struct WorldsFound
	{
		void* at[8];
		int count;
	};

	void take_world(void* header, void* data)
	{
		auto* found = static_cast<WorldsFound*>(data);
		if (found->count < 8)
		{
			found->at[found->count++] = header;
		}
	}
}

int theater_sky_images(void** out, const int max, char* world, const size_t size)
{
	__try
	{
		WorldsFound found;
		found.count = 0;
		reinterpret_cast<void (*)(int, void (*)(void*, void*), const void*, bool)>(DB_ENUM_XASSETS)(ASSET_GFX_WORLD, take_world, &found, false);
		for (int w = 0; w < found.count; ++w)
		{
			const auto* at = static_cast<const unsigned char*>(found.at[w]);
			if (IsBadReadPtr(at, 0x30))
			{
				continue;
			}

			const char* name = *reinterpret_cast<const char* const*>(at);
			const std::int32_t count = *reinterpret_cast<const std::int32_t*>(at + 0x20);
			const auto* skies = *reinterpret_cast<const unsigned char* const*>(at + 0x28);
			if (count < 1 || count > 16 || IsBadReadPtr(skies, 56))
			{
				log_line("sky: world %d of %d has %d skies at %p (not taken)", w, found.count, count, skies);
				continue;
			}

			int taken = 0;
			for (int i = 0; i < count && taken < max; ++i)
			{
				if (IsBadReadPtr(skies + i * 56, 56))
				{
					break;
				}

				void* image = *reinterpret_cast<void* const*>(skies + i * 56 + 0x10);
				if (image && !IsBadReadPtr(image, 0x68))
				{
					out[taken++] = image;
				}
			}

			if (name && !IsBadReadPtr(name, 1))
			{
				strncpy_s(world, size, name, _TRUNCATE);
			}

			log_line("sky: world %s has %d skies, %d images taken", world, count, taken);
			if (taken > 0)
			{
				return taken;
			}
		}

		return found.count ? 0 : -1;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		log_line("sky: a fault looking for the map's sky");
		return -2;
	}
}

void* theater_dvar(const char* name)
{
	return reinterpret_cast<void* (*)(const char*)>(DVAR_FIND_VAR)(name);
}

int theater_dvar_count()
{
	const int count = *reinterpret_cast<const int*>(DVAR_COUNT);
	return count < 0 ? 0 : count > 20000 ? 20000 : count;
}

void* theater_dvar_at(const int index)
{
	return reinterpret_cast<void**>(SORTED_DVARS)[index];
}

const char* theater_folder() { return g_folder; }
const char* theater_version() { return VERSION; }

namespace
{
	// How long a recording is: the time of its last message, read from the file's record headers.
	std::uint32_t recording_length(const char* path)
	{
		FILE* file = nullptr;
		if (fopen_s(&file, path, "rb") != 0 || !file)
		{
			return 0;
		}

		std::uint32_t header[3] = {}, last = 0;
		if (fread(header, sizeof(header), 1, file) == 1 && header[0] == 0x4D445741 && header[2] == MSG_SIZE)
		{
			std::uint32_t head[3];
			while (fread(head, sizeof(head), 1, file) == 1 && head[2] != 0 && head[2] <= MAX_MESSAGE)
			{
				const bool message = head[1] != KIND_PREDICTED && head[1] != KIND_PLAYER_STATE;
				if (message)
				{
					last = head[0];
				}

				if (fseek(file, static_cast<long>(head[2] + (message ? MSG_SIZE : 0)), SEEK_CUR) != 0)
				{
					break;
				}
			}
		}

		fclose(file);
		return last;
	}
}

int theater_recordings(RecordingInfo* out, const int max)
{
	// (lengths are kept by file name and size: reading them takes a moment)
	struct Known
	{
		std::string file;
		std::uint64_t size;
		std::uint32_t length;
	};
	static std::vector<Known> known;
	char pattern[MAX_PATH];
	sprintf_s(pattern, "%s\\*.awdm.txt", g_folder);
	WIN32_FIND_DATAA found;
	const HANDLE search = FindFirstFileA(pattern, &found);
	if (search == INVALID_HANDLE_VALUE)
	{
		return 0;
	}

	// (the named ones too; newest first by when they were written)
	std::vector<std::pair<std::uint64_t, std::string>> dated;
	do
	{
		std::string name = found.cFileName;
		dated.push_back({(static_cast<std::uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime,
		                 name.substr(0, name.size() - 4)});
	} while (FindNextFileA(search, &found));
	FindClose(search);
	std::sort(dated.begin(), dated.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::vector<std::string> names;
	for (const auto& entry : dated)
	{
		names.push_back(entry.second);
	}
	int count = 0;
	for (const std::string& name : names)
	{
		if (count >= max)
		{
			break;
		}

		RecordingInfo& info = out[count];
		info = RecordingInfo();
		strncpy_s(info.file, name.c_str(), _TRUNCATE);
		char path[MAX_PATH], info_path[MAX_PATH];
		sprintf_s(path, "%s\\%s", g_folder, name.c_str());
		sprintf_s(info_path, "%s.txt", path);
		WIN32_FILE_ATTRIBUTE_DATA data{};
		if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
		{
			continue;
		}

		info.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
		FILE* file = nullptr;
		if (fopen_s(&file, info_path, "r") == 0 && file)
		{
			char key[32], value[64];
			while (fscanf_s(file, "%31s %63s", key, static_cast<unsigned>(sizeof(key)), value, static_cast<unsigned>(sizeof(value))) == 2)
			{
				strncpy_s(strcmp(key, "map") == 0 ? info.map : info.gametype, sizeof(info.map), value, _TRUNCATE);
			}

			fclose(file);
		}

		bool have = false;
		for (const Known& k : known)
		{
			if (k.file == name && k.size == info.size)
			{
				info.length_ms = k.length;
				have = true;
			}
		}

		if (!have)
		{
			info.length_ms = recording_length(path);
			known.push_back({name, info.size, info.length_ms});
		}

		++count;
	}

	return count;
}

void theater_play(const char* file)
{
	strncpy_s(g_wanted_name, file, _TRUNCATE);
	// one playing now is stopped first, and the game given a moment to leave its match
	const bool playing = g_play != Play::Idle;
	if (playing)
	{
		g_stop_request = true;
	}

	g_wanted_after = GetTickCount64() + (playing ? 3000 : 0);
	g_wanted = true;
	log_line("asked for: %s", file);
}

bool theater_idle()
{
	return g_play == Play::Idle && !g_file && !g_wanted;
}

void theater_command(const char* text)
{
	std::lock_guard<std::mutex> lock(g_commands_lock);
	g_commands.emplace_back(text);
}

void theater_hud_wake()
{
	g_hud_real_until = GetTickCount64() + 500;
}

// S1x's console has the keys while it is open (bit 1 of the game's key catchers, as s1-mod's
// game_console.cpp sets it): what is typed there is not the editor's.
bool theater_keys_blocked()
{
	return (*reinterpret_cast<const int*>(0x1417E168C) & 1) != 0 || ui_typing();
}
HMODULE theater_module() { return g_module; }
void theater_log(const char* text) { log_line("%s", text); }

void theater_start(const HMODULE self)
{
	if (g_started.exchange(true))
	{
		return;
	}

	g_module = self;

	// the folder this DLL is in is the game's
	char folder[MAX_PATH] = {};
	GetModuleFileNameA(self, folder, MAX_PATH);
	if (char* slash = strrchr(folder, '\\'))
	{
		*slash = 0;
	}

	sprintf_s(g_folder, "%s\\theater", folder);
	CreateDirectoryA(g_folder, nullptr);
	char log_path[MAX_PATH];
	sprintf_s(log_path, "%s\\s1iris.log", g_folder);
	// a long log starts over (the last one is kept beside it); it can be read while the game runs
	WIN32_FILE_ATTRIBUTE_DATA log_info{};
	if (GetFileAttributesExA(log_path, GetFileExInfoStandard, &log_info) && (log_info.nFileSizeHigh || log_info.nFileSizeLow > 600000))
	{
		char previous[MAX_PATH];
		sprintf_s(previous, "%s\\s1iris_previous.log", g_folder);
		MoveFileExA(log_path, previous, MOVEFILE_REPLACE_EXISTING);
	}

	g_log = _fsopen(log_path, "a", _SH_DENYNO);
	InitializeCriticalSection(&g_lock);
	InitializeCriticalSection(&g_play_lock);
	log_line("---- S1iris %s loaded ----", VERSION);
	reserve_hook_memory();
	if (HANDLE thread = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr))
	{
		CloseHandle(thread);
	}
}
