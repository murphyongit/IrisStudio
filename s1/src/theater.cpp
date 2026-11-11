// S1iris: a theater mode for Advanced Warfare multiplayer (S1x), step one: the recorder's tap.
//
// Advanced Warfare has no demo system. A demo is the stream of messages the server sends the client,
// kept in a file and given to the client again later (as the Ghosts demo system of anomaly/iw6-mod and
// H1MVM's port of it do). This build only proves the tap on this game: it hooks CL_ParseServerMessage and
// writes every message it is given to theater\rec_<date>_<time>.awdm, with what it did in
// theater\s1iris.log. It plays nothing back and changes nothing in the game.
//
// Addresses are those of s1_mp64_ship.exe (linker timestamp 1563218830), found in its unpacked code.

#include "theater.hpp"

#include <MinHook.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
	constexpr char VERSION[] = "0.2 (recorder tap)";

	// void CL_ParseServerMessage(int localClientNum, msg_t* msg)
	constexpr uintptr_t CL_PARSE_SERVER_MESSAGE = 0x1402129B0;
	// its first bytes once the game has unpacked itself: mov [rsp+10h],rbx ; mov [rsp+18h],rdi ; push rbp
	constexpr unsigned char PROLOGUE[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55};

	// what is read of the game's msg_t
	constexpr size_t MSG_SIZE = 0x38;
	constexpr size_t MSG_DATA = 0x08;
	constexpr size_t MSG_CURSIZE = 0x1C;
	constexpr size_t MSG_READCOUNT = 0x24;
	constexpr unsigned MAX_MESSAGE = 0x20000;

	// a pause this long between messages is a new match: a new file
	constexpr ULONGLONG NEW_FILE_GAP_MS = 5000;

	char g_folder[MAX_PATH] = {};
	CRITICAL_SECTION g_lock;
	FILE* g_log = nullptr;
	FILE* g_file = nullptr;
	ULONGLONG g_file_start = 0, g_last_message = 0, g_last_flush = 0, g_last_report = 0;
	std::uint64_t g_file_messages = 0, g_file_bytes = 0, g_faults = 0;
	std::atomic<bool> g_started{false};

	using parse_t = void (*)(int, void*);
	parse_t g_original_parse = nullptr;

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
		if (fopen_s(&g_file, path, "wb") != 0 || !g_file)
		{
			g_file = nullptr;
			log_line("could not create %s", path);
			return false;
		}

		// header: "AWDM", format 1, the size of the msg_t kept with each message
		const std::uint32_t header[3] = {0x4D445741, 1, static_cast<std::uint32_t>(MSG_SIZE)};
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

	void hooked_parse(const int client, void* msg)
	{
		EnterCriticalSection(&g_lock);
		record(client, msg);
		LeaveCriticalSection(&g_lock);
		g_original_parse(client, msg);
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

		log_line(status == MH_OK ? "hook: server messages installed. Play a private match; it is recorded by itself."
		                         : "hook: server messages FAILED (%s)",
		         MH_StatusToString(status));
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
	log_line("---- S1iris %s loaded ----", VERSION);
	reserve_hook_memory();
	if (HANDLE thread = CreateThread(nullptr, 0, &worker, nullptr, 0, nullptr))
	{
		CloseHandle(thread);
	}
}
