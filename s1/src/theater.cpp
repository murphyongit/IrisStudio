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

#include <MinHook.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	constexpr char VERSION[] = "0.13 (recorder, playback with rewind, crash notes)";

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
	constexpr float SPEEDS[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
	constexpr int NORMAL_SPEED = 3;
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
	// a client game's snapshot: its flags, of which this one changes when the map starts again
	constexpr size_t SNAPSHOT_FLAGS = 0x5B68;
	constexpr std::int32_t SNAPFLAG_SERVERCOUNT = 4;
	// CG_SetInitialSnapshot (0x1401C2200) ends by taking away the snapshot the client game is going to
	// (`mov qword ptr [cg->nextSnap], 0`, 11 bytes): until the next one is read there is none, and code
	// that runs between reads it without looking (the crashes at 0x1401AFEEA after going back). With these
	// bytes gone it stays the snapshot the client game is at, which CG_ProcessSnapshots takes to mean
	// "no next one yet" just as well.
	constexpr uintptr_t CG_INITIAL_SNAPSHOT_CLEARS_NEXT = 0x1401C23D6;
	constexpr unsigned char CLEARS_NEXT[] = {0x48, 0xC7, 0x05, 0x2F, 0x43, 0x30, 0x01, 0x00, 0x00, 0x00, 0x00};
	constexpr std::uint32_t BACK_MS = 10000;
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
	constexpr int HOOK_DELAY_S = 25;
	// the match's map is surely the game's by this many messages (2 s)
	constexpr std::uint64_t MATCH_INFO_AT = 40;

	// a pause this long between messages is a new match: a new file
	constexpr ULONGLONG NEW_FILE_GAP_MS = 5000;

	bool read_bytes_forward(uintptr_t address, void* out, size_t count);
	void keep_next_snapshot(bool keep);

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
	std::vector<Predicted> g_predicted;
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
	std::atomic<bool> g_back_request{false}, g_restart_request{false};
	// the client's time is to be set to the next snapshot's, once one has been given after going back
	bool g_resync_time = false;
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

	void close_file()
	{
		if (g_file)
		{
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
		if (fopen_s(&g_file, path, "wb") != 0 || !g_file)
		{
			g_file = nullptr;
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
				Predicted predicted;
				if (head[2] != sizeof(predicted) || fread(&predicted, sizeof(predicted), 1, file) != 1)
				{
					break;
				}

				if (g_predicted.empty() || predicted.serverTime > g_predicted.back().serverTime)
				{
					g_predicted.push_back(predicted);
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

	void stop_playback(const char* why)
	{
		if (g_play == Play::Idle)
		{
			return;
		}

		log_line("playback ended (%s): %zu of %zu messages given, %llu of the local match's dropped", why, g_next_record,
		         g_records.size(), g_dropped);
		keep_next_snapshot(false);
		g_play = Play::Idle;
		g_records.clear();
		g_records.shrink_to_fit();
		g_predicted.clear();
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
		if (!find_newest(g_play_name) || !load_recording(g_play_name, map, type))
		{
			log_line("playback: no recording with its map beside it (one recorded with this version) was found");
			return;
		}

		g_predicted_given = 0;
		g_predicted_missed = 0;
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
	bool g_keeps_next = false;

	// The client game keeps a snapshot to go to through a new first snapshot (see CLEARS_NEXT), or not.
	void keep_next_snapshot(const bool keep)
	{
		if (keep == g_keeps_next)
		{
			return;
		}

		unsigned char now[sizeof(CLEARS_NEXT)];
		unsigned char nops[sizeof(CLEARS_NEXT)];
		memset(nops, 0x90, sizeof(nops));
		if (!read_bytes(CG_INITIAL_SNAPSHOT_CLEARS_NEXT, now, sizeof(now)) ||
		    memcmp(now, keep ? CLEARS_NEXT : nops, sizeof(now)) != 0)
		{
			log_line("playback: the client game's code is not as expected at its first snapshot; left as it is");
			return;
		}

		DWORD old = 0;
		if (VirtualProtect(reinterpret_cast<LPVOID>(CG_INITIAL_SNAPSHOT_CLEARS_NEXT), sizeof(now), PAGE_EXECUTE_READWRITE, &old))
		{
			memcpy(reinterpret_cast<void*>(CG_INITIAL_SNAPSHOT_CLEARS_NEXT), keep ? nops : CLEARS_NEXT, sizeof(now));
			VirtualProtect(reinterpret_cast<LPVOID>(CG_INITIAL_SNAPSHOT_CLEARS_NEXT), sizeof(now), old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(CG_INITIAL_SNAPSHOT_CLEARS_NEXT), sizeof(now));
			g_keeps_next = keep;
			log_line("playback: the client game %s", keep ? "keeps a snapshot to go to through a new first one" : "is as the game has it again");
		}
	}

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
			// The snapshots the client game is at stay its (code between frames reads them: 0.11 took
			// them away and the game crashed at 0x1401AFEEA). The one it is going to is marked as of
			// another start of the map (the flag a map restart changes), which is how the game itself
			// takes a snapshot older than its own: it begins again from it.
			if (auto* next = *reinterpret_cast<unsigned char**>(CG_NEXT_SNAP))
			{
				*reinterpret_cast<std::int32_t*>(next + SNAPSHOT_FLAGS) ^= SNAPFLAG_SERVERCOUNT;
			}

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
	void go_back_to(const std::uint64_t ms)
	{
		g_faults_noted = 0;
		keep_next_snapshot(true);
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
		g_resync_time = true;
		log_line("playback: back to %.1f s", ms / 1000.0);
	}

	// Playback's step. On the main thread whenever the client has a message from the local match or would
	// send its packet; from the key thread (main false) only what cannot wait for those: a start or a stop
	// while nothing is being played.
	void play_step(const bool main)
	{
		if (g_stop_request && (main || g_play != Play::Playing))
		{
			g_stop_request = false;
			stop_playback("F10");
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
				g_resync_time = false;
				g_play = Play::Playing;
				g_play_start = now;
				g_play_report = now;
				log_line("playback: the client is ready (connection state %d); the recording is its server from here", state);
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

		if (g_skip_request.exchange(false))
		{
			g_clock += SKIP_MS;
			show("S1iris: 10 seconds on");
		}

		if (g_back_request.exchange(false))
		{
			const std::uint64_t clock = g_clock;
			go_back_to(clock > BACK_MS ? clock - BACK_MS : 0);
			show("S1iris: 10 seconds back");
		}

		if (g_restart_request.exchange(false))
		{
			go_back_to(0);
			show("S1iris: from the start");
		}

		if (g_log_ticks > 0)
		{
			--g_log_ticks;
			log_stream("tick");
		}

		const auto elapsed = static_cast<std::uint32_t>(g_clock.load());
		for (int given = 0; given < 100 && g_next_record < g_records.size() && g_records[g_next_record].ms <= elapsed; ++given)
		{
			if (!feed_guarded(g_records[g_next_record]))
			{
				log_line("playback: message %zu could not be parsed (a fault in the game)", g_next_record);
				stop_playback("a message the game could not take");
				return;
			}

			++g_next_record;
			if (g_resync_time)
			{
				resync_time();
			}
		}

		if (now - g_play_report > 5000)
		{
			g_play_report = now;
			log_line("playback: %.0f s (speed %gx%s), %zu of %zu messages given, connection state %d, %llu local messages dropped, "
			         "%llu bots refused, %llu server steps skipped; the player's position given %llu times, not found %llu; demo mode %s",
			         elapsed / 1000.0, SPEEDS[g_speed], g_paused ? ", paused" : "", g_next_record, g_records.size(),
			         connection_state(), g_dropped, g_bots_refused.load(),
			         g_server_frames_skipped.load(), g_predicted_given.load(), g_predicted_missed.load(),
			         g_demo_mode ? "on" : "off");
		}

		if (g_next_record >= g_records.size())
		{
			stop_playback("the recording's end");
		}
	}

	void play_tick(const bool main = true)
	{
		EnterCriticalSection(&g_play_lock);
		play_step(main);
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

		play_tick();
		if (g_play == Play::Playing)
		{
			// the local match's: the recording is the server now
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
		play_tick();
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
	int hooked_time_scale_msec(const int msec)
	{
		if (g_play != Play::Playing)
		{
			return g_original_time_scale_msec(msec);
		}

		if (g_paused || msec <= 0)
		{
			return 0;
		}

		g_clock_fraction += static_cast<float>(msec > 200 ? 200 : msec) * SPEEDS[g_speed];
		const int whole = static_cast<int>(g_clock_fraction);
		g_clock_fraction -= static_cast<float>(whole);
		g_clock += static_cast<std::uint64_t>(whole);
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
			                               static_cast<std::uint32_t>(sizeof(predicted))};
			fwrite(head, sizeof(head), 1, g_file);
			fwrite(&predicted, sizeof(predicted), 1, g_file);
		}

		LeaveCriticalSection(&g_lock);
	}

	// Playback: the recorded player's position of a time, where the game asks for what it predicted.
	bool hooked_get_predicted(unsigned char* cl, const int server_time, unsigned char* ps)
	{
		const bool found = g_original_get_predicted(cl, server_time, ps);
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

		const auto& predicted = g_predicted[low - 1];
		memcpy(ps + PS_ORIGIN, predicted.origin, sizeof(predicted.origin));
		memcpy(ps + PS_VELOCITY, predicted.velocity, sizeof(predicted.velocity));
		ps[PS_BOB_CYCLE] = static_cast<unsigned char>(predicted.bobCycle);
		memcpy(ps + PS_MOVEMENT_DIR, &predicted.movementDir, sizeof(predicted.movementDir));
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
			const bool playing = focus && g_play == Play::Playing;
			const bool down_now = playing && (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
			const bool left_now = playing && (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
			const bool right_now = playing && (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
			const bool up_now = playing && (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
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

			const bool page_down_now = playing && (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
			const bool home_now = playing && (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
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
				log_line("hook: playback's pause and speed %s (down arrow pauses, left and right change speed, up skips 10 s, Page Down goes back 10 s, Home to the start)",
				         time ? "yes" : "NO");
				const bool save = hook(CL_SAVE_PREDICTED, &hooked_save_predicted, &g_original_save_predicted, "the player's position, kept");
				const bool get = hook(CL_GET_PREDICTED, &hooked_get_predicted, &g_original_get_predicted, "the player's position, asked for");
				const bool draw = hook(CG_DRAW_ACTIVE_FRAME, &hooked_draw_active_frame, &g_original_draw_active_frame, "the frame");
				log_line("hook: the player's own view: recorded %s, played back %s, demo mode %s (F8 switches it while playing)",
				         save ? "yes" : "NO", get ? "yes" : "NO", draw ? "yes" : "NO");
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

void theater_start(const HMODULE self)
{
	if (g_started.exchange(true))
	{
		return;
	}

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
	fopen_s(&g_log, log_path, "a");
	InitializeCriticalSection(&g_lock);
	InitializeCriticalSection(&g_play_lock);
	log_line("---- S1iris %s loaded ----", VERSION);
	reserve_hook_memory();
	if (HANDLE thread = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr))
	{
		CloseHandle(thread);
	}
}
