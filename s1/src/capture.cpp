// The Record tab: the recording played into a video, frame by frame.
//
// While it records, playback's clock goes on by exactly one frame of the chosen rate for every frame the
// game draws (theater_capture), so the video is even however long a frame takes to draw and write. Each
// frame is the game's picture as it was drawn, before the editor is drawn over it (ui.cpp's copy), read
// back from the graphics card and handed to a writing thread.
//
// The frames go to FFmpeg as raw video, as IrisStudio does it (core/src/Components/CaptureManager.cpp:
// the same places are looked in for ffmpeg.exe, the same ProRes settings). Without FFmpeg the frames are
// written as numbered TGA pictures.
#include "ui_parts.hpp"
#include "passes.hpp"

#include <shellapi.h>

#include <atomic>
#include <cfloat>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	enum class Format { Video, ImageSequence };
	enum class Step { Idle, Seeking, Recording, Finishing };

	struct Codec
	{
		const char* label;
		const char* arguments;
		const char* extension;
	};

	// IrisStudio's (ProRes by profile), and H.264 for a small file
	const Codec CODECS[] = {
		{"ProRes 4444 XQ", "-c:v prores -profile:v 5 -q:v 1 -pix_fmt yuv444p10le", "mov"},
		{"ProRes 4444", "-c:v prores -profile:v 4 -q:v 1 -pix_fmt yuv444p10le", "mov"},
		{"ProRes 422 HQ", "-c:v prores -profile:v 3 -q:v 1 -pix_fmt yuv422p10le", "mov"},
		{"ProRes 422", "-c:v prores -profile:v 2 -q:v 1 -pix_fmt yuv422p10le", "mov"},
		{"ProRes 422 LT", "-c:v prores -profile:v 1 -q:v 1 -pix_fmt yuv422p10le", "mov"},
		{"H.264 (small file)", "-c:v libx264 -preset medium -crf 14 -pix_fmt yuv420p", "mp4"},
	};

	struct Size
	{
		const char* label;
		int width, height;
	};

	const Size SIZES[] = {{"The game's own", 0, 0}, {"3840x2160", 3840, 2160}, {"2560x1440", 2560, 1440}, {"1920x1080", 1920, 1080},
	                      {"1280x720", 1280, 720}};
	const int RATES[] = {24, 25, 30, 50, 60, 120, 125, 240, 250, 500, 1000};

	// what the tab is set to
	Format g_format = Format::Video;
	int g_codec = 2, g_size = 0, g_rate = 60;
	float g_start = 0.0f, g_end = 0.0f;
	std::string g_timeframe_for;

	// the recording under way
	std::atomic<Step> g_step{Step::Idle};
	std::atomic<bool> g_stop{false};
	int g_waited = 0, g_skip = 0, g_settled_frames = 0;
	std::atomic<int> g_frames{0};
	std::string g_output, g_message;
	ID3D11Texture2D* g_staging = nullptr;
	D3D11_TEXTURE2D_DESC g_staging_desc{};

	// the writer
	std::thread g_writer;
	std::mutex g_lock;
	std::condition_variable g_wake;
	std::vector<std::vector<unsigned char>> g_queue;
	bool g_writer_done = false, g_writer_failed = false;
	HANDLE g_pipe = nullptr, g_process = nullptr;
	int g_width = 0, g_height = 0, g_bytes_per_pixel = 4;
	DXGI_FORMAT g_picture_format = DXGI_FORMAT_UNKNOWN;
	std::string g_sequence_folder;

	std::string captures_folder()
	{
		std::string folder = std::string(theater_folder()) + "\\captures";
		CreateDirectoryA(folder.c_str(), nullptr);
		return folder;
	}

	bool exists(const std::string& path)
	{
		const DWORD attributes = GetFileAttributesA(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	// Where IrisStudio keeps the FFmpeg it downloads, then beside the game, then anywhere Windows finds it.
	std::string look_for_ffmpeg();

	std::string find_ffmpeg()
	{
		// (looked for again every few seconds: it may be installed while the game runs)
		static std::string found;
		static ULONGLONG looked = 0;
		const ULONGLONG now = GetTickCount64();
		if (!looked || now - looked > 3000)
		{
			looked = now;
			found = look_for_ffmpeg();
		}

		return found;
	}

	std::string look_for_ffmpeg()
	{
		char local[MAX_PATH] = {}, roaming[MAX_PATH] = {};
		GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH);
		GetEnvironmentVariableA("APPDATA", roaming, MAX_PATH);
		const std::string theater = theater_folder();
		const std::string places[] = {std::string(local) + "\\IrisStudio\\ffmpeg.exe", std::string(local) + "\\bo1mvm\\ffmpeg.exe",
		                              std::string(roaming) + "\\codmvm_launcher\\ffmpeg.exe", theater + "\\ffmpeg.exe",
		                              theater + "\\..\\ffmpeg.exe"};
		for (const std::string& place : places)
		{
			if (exists(place))
			{
				return place;
			}
		}

		char found[MAX_PATH] = {};
		return SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) ? std::string(found) : std::string();
	}

	const char* pixel_format(const DXGI_FORMAT format)
	{
		switch (format)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM: return "rgba";
		case DXGI_FORMAT_B8G8R8A8_UNORM: return "bgra";
		case DXGI_FORMAT_R10G10B10A2_UNORM: return "x2bgr10le";
		default: return nullptr;
		}
	}

	// A frame as a TGA picture (24 bits, top row first).
	void write_tga(const std::vector<unsigned char>& frame, const int number)
	{
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s\\frame_%06d.tga", g_sequence_folder.c_str(), number);
		FILE* file = nullptr;
		if (fopen_s(&file, path, "wb") || !file)
		{
			g_writer_failed = true;
			return;
		}

		unsigned char header[18] = {};
		header[2] = 2;
		header[12] = static_cast<unsigned char>(g_width & 0xFF);
		header[13] = static_cast<unsigned char>(g_width >> 8);
		header[14] = static_cast<unsigned char>(g_height & 0xFF);
		header[15] = static_cast<unsigned char>(g_height >> 8);
		header[16] = 24;
		header[17] = 0x20;
		fwrite(header, sizeof(header), 1, file);
		std::vector<unsigned char> row(static_cast<size_t>(g_width) * 3);
		for (int y = 0; y < g_height; ++y)
		{
			const unsigned char* in = frame.data() + static_cast<size_t>(y) * g_width * 4;
			for (int x = 0; x < g_width; ++x, in += 4)
			{
				unsigned char r, g, b;
				if (g_picture_format == DXGI_FORMAT_R10G10B10A2_UNORM)
				{
					const unsigned packed = *reinterpret_cast<const unsigned*>(in);
					r = static_cast<unsigned char>((packed & 0x3FF) >> 2);
					g = static_cast<unsigned char>(((packed >> 10) & 0x3FF) >> 2);
					b = static_cast<unsigned char>(((packed >> 20) & 0x3FF) >> 2);
				}
				else if (g_picture_format == DXGI_FORMAT_B8G8R8A8_UNORM)
				{
					b = in[0];
					g = in[1];
					r = in[2];
				}
				else
				{
					r = in[0];
					g = in[1];
					b = in[2];
				}

				row[static_cast<size_t>(x) * 3] = b;
				row[static_cast<size_t>(x) * 3 + 1] = g;
				row[static_cast<size_t>(x) * 3 + 2] = r;
			}

			fwrite(row.data(), row.size(), 1, file);
		}

		fclose(file);
	}

	void writer()
	{
		int number = 0;
		for (;;)
		{
			std::vector<unsigned char> frame;
			{
				std::unique_lock<std::mutex> lock(g_lock);
				g_wake.wait(lock, [] { return !g_queue.empty() || g_writer_done; });
				if (g_queue.empty())
				{
					break;
				}

				frame = std::move(g_queue.front());
				g_queue.erase(g_queue.begin());
			}

			g_wake.notify_all();
			if (g_writer_failed)
			{
				continue;
			}

			if (g_pipe)
			{
				size_t done = 0;
				while (done < frame.size())
				{
					DWORD written = 0;
					const DWORD chunk = static_cast<DWORD>(frame.size() - done > (1u << 24) ? (1u << 24) : frame.size() - done);
					if (!WriteFile(g_pipe, frame.data() + done, chunk, &written, nullptr) || !written)
					{
						g_writer_failed = true;
						break;
					}

					done += written;
				}
			}
			else
			{
				write_tga(frame, ++number);
			}
		}
	}

	bool start_ffmpeg(const std::string& ffmpeg, const std::string& folder, const char* name)
	{
		const char* pixels = pixel_format(g_picture_format);
		if (!pixels)
		{
			g_message = "The game's picture is in a format FFmpeg is not given here; record an image sequence instead.";
			return false;
		}

		const Size& size = SIZES[g_size];
		char scale[64] = "";
		if (size.width)
		{
			snprintf(scale, sizeof(scale), "-vf scale=%d:%d:flags=lanczos ", size.width, size.height);
		}

		char output[MAX_PATH];
		const Codec& codec = CODECS[g_codec];
		if (g_format == Format::Video)
		{
			for (int n = 1;; ++n)
			{
				snprintf(output, sizeof(output), "%s\\%s_%d.%s", folder.c_str(), name, n, codec.extension);
				if (!exists(output))
				{
					break;
				}
			}
		}
		else
		{
			snprintf(output, sizeof(output), "%s\\frame_%%06d.tga", g_sequence_folder.c_str());
		}

		g_output = g_format == Format::Video ? output : g_sequence_folder;
		char command[2048];
		snprintf(command, sizeof(command), "\"%s\" -hide_banner -loglevel error -f rawvideo -pix_fmt %s -s %dx%d -r %d -i - %s%s -y \"%s\"",
		         ffmpeg.c_str(), pixels, g_width, g_height, g_rate, scale, g_format == Format::Video ? codec.arguments : "-q:v 0", output);
		ui_log("record: %s", command);

		SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
		HANDLE read_end = nullptr, write_end = nullptr;
		if (!CreatePipe(&read_end, &write_end, &inherit, 64 << 20))
		{
			g_message = "The pipe to FFmpeg could not be made.";
			return false;
		}

		SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, 0);
		const std::string log_path = folder + "\\ffmpeg_log.txt";
		const HANDLE log_file = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		STARTUPINFOA startup{sizeof(STARTUPINFOA)};
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = read_end;
		startup.hStdOutput = log_file != INVALID_HANDLE_VALUE ? log_file : nullptr;
		startup.hStdError = startup.hStdOutput;
		PROCESS_INFORMATION process{};
		const BOOL started = CreateProcessA(nullptr, command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &startup, &process);
		CloseHandle(read_end);
		if (log_file != INVALID_HANDLE_VALUE)
		{
			CloseHandle(log_file);
		}

		if (!started)
		{
			CloseHandle(write_end);
			g_message = "FFmpeg could not be started.";
			return false;
		}

		CloseHandle(process.hThread);
		g_process = process.hProcess;
		g_pipe = write_end;
		return true;
	}

	// The writer is started for a picture of this size and format.
	bool begin_output(const TheaterState& state)
	{
		const std::string folder = captures_folder();
		char name[128];
		snprintf(name, sizeof(name), "%s", state.name);
		if (char* dot = strchr(name, '.'))
		{
			*dot = 0;
		}

		// (a pass has its word in the file's name)
		if (passes_tag()[0])
		{
			const size_t length = strlen(name);
			snprintf(name + length, sizeof(name) - length, "_%s", passes_tag());
		}

		g_pipe = nullptr;
		g_process = nullptr;
		g_frames = 0;
		g_writer_done = g_writer_failed = false;
		g_queue.clear();
		if (g_format == Format::ImageSequence)
		{
			for (int n = 1;; ++n)
			{
				g_sequence_folder = folder + "\\" + name + "_frames_" + std::to_string(n);
				if (CreateDirectoryA(g_sequence_folder.c_str(), nullptr))
				{
					break;
				}

				if (GetLastError() != ERROR_ALREADY_EXISTS || n > 9999)
				{
					g_message = "The folder for the pictures could not be made.";
					return false;
				}
			}
		}

		const std::string ffmpeg = find_ffmpeg();
		if (g_format == Format::Video || (!ffmpeg.empty() && SIZES[g_size].width))
		{
			if (ffmpeg.empty())
			{
				g_message = "FFmpeg was not found, and a video needs it. Record an image sequence, or start Iris Studio once (it downloads FFmpeg).";
				return false;
			}

			if (!start_ffmpeg(ffmpeg, folder, name))
			{
				return false;
			}
		}
		else
		{
			g_output = g_sequence_folder;
		}

		g_writer = std::thread(&writer);
		return true;
	}

	void end_output()
	{
		{
			std::lock_guard<std::mutex> lock(g_lock);
			g_writer_done = true;
		}

		g_wake.notify_all();
		if (g_writer.joinable())
		{
			g_writer.join();
		}

		if (g_pipe)
		{
			CloseHandle(g_pipe);
			g_pipe = nullptr;
		}

		if (g_process)
		{
			// FFmpeg finishes the file once its input ends
			WaitForSingleObject(g_process, 20000);
			CloseHandle(g_process);
			g_process = nullptr;
		}
	}

	// the passes still to be recorded after the one under way (each is the same stretch again)
	std::vector<int> g_passes_left;
	bool g_passes_run = false;

	void finish(const char* why)
	{
		theater_capture(0.0f);
		theater_set_paused(true);
		end_output();
		char text[400];
		snprintf(text, sizeof(text), "%s %d frames written to %s%s", why, g_frames.load(), g_output.c_str(),
		         g_writer_failed ? " (the writing failed part way: see ffmpeg_log.txt in the captures folder)" : "");
		g_message = text;
		ui_log("record: %s", text);
		const bool go_on = g_passes_run && !g_passes_left.empty() && !g_stop && !g_writer_failed && strcmp(why, "Done.") == 0;
		if (go_on)
		{
			const int next = g_passes_left.front();
			g_passes_left.erase(g_passes_left.begin());
			passes_use(next);
			ui_log("record: next pass: %s", passes_label(next));
			g_waited = 0;
			g_settled_frames = 0;
			theater_seek(static_cast<std::uint32_t>(g_start * 1000.0f));
			g_step = Step::Seeking;
			return;
		}

		if (g_passes_run)
		{
			// (the whole picture again once the passes are done)
			g_passes_run = false;
			g_passes_left.clear();
			passes_use(-1);
		}

		g_step = Step::Idle;
	}

	void take_frame(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* picture)
	{
		D3D11_TEXTURE2D_DESC desc{};
		picture->GetDesc(&desc);
		if (!g_staging || g_staging_desc.Width != desc.Width || g_staging_desc.Height != desc.Height)
		{
			if (g_staging)
			{
				g_staging->Release();
				g_staging = nullptr;
			}

			g_staging_desc = desc;
			g_staging_desc.Format = g_picture_format;
			g_staging_desc.Usage = D3D11_USAGE_STAGING;
			g_staging_desc.BindFlags = 0;
			g_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			g_staging_desc.MiscFlags = 0;
			if (FAILED(device->CreateTexture2D(&g_staging_desc, nullptr, &g_staging)) || !g_staging)
			{
				g_writer_failed = true;
				return;
			}
		}

		context->CopyResource(g_staging, picture);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped)))
		{
			g_writer_failed = true;
			return;
		}

		std::vector<unsigned char> frame(static_cast<size_t>(g_width) * g_height * 4);
		for (int y = 0; y < g_height; ++y)
		{
			memcpy(frame.data() + static_cast<size_t>(y) * g_width * 4, static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
			       static_cast<size_t>(g_width) * 4);
		}

		context->Unmap(g_staging, 0);
		{
			// the writer is waited for rather than a frame dropped: the clock only goes on when a frame is drawn
			std::unique_lock<std::mutex> lock(g_lock);
			g_wake.wait(lock, [] { return g_queue.size() < 6 || g_writer_failed; });
			g_queue.push_back(std::move(frame));
		}

		g_wake.notify_all();
		++g_frames;
	}
}

bool capture_wants_picture()
{
	return g_step != Step::Idle;
}

void capture_frame(const TheaterState& state, ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* picture, const DXGI_FORMAT format)
{
	const Step step = g_step;
	if (step == Step::Idle)
	{
		return;
	}

	if (!state.playing)
	{
		finish("Playback ended.");
		return;
	}

	if (step == Step::Seeking)
	{
		// held still at the start until the picture has arrived there
		if (++g_waited > 1200)
		{
			theater_capture(0.0f);
			g_message = "The recording did not get to the start in time.";
			g_step = Step::Idle;
			return;
		}

		if (g_waited < 12 || !state.settled || !state.paused || !picture)
		{
			g_settled_frames = 0;
			return;
		}

		// a little longer once every message is in: the client game takes a few frames to get there
		if (++g_settled_frames < 20)
		{
			return;
		}

		D3D11_TEXTURE2D_DESC desc{};
		picture->GetDesc(&desc);
		g_width = static_cast<int>(desc.Width);
		g_height = static_cast<int>(desc.Height);
		g_picture_format = format;
		if (!begin_output(state))
		{
			g_step = Step::Idle;
			return;
		}

		theater_capture(static_cast<float>(g_rate));
		theater_set_paused(false);
		g_skip = 2;  // the frames already on their way were drawn before the clock was stepped
		g_stop = false;
		g_step = Step::Recording;
		return;
	}

	if (step == Step::Recording)
	{
		if (g_skip > 0)
		{
			--g_skip;
			return;
		}

		if (picture && !state.paused)
		{
			take_frame(device, context, picture);
		}

		const bool ended = state.position_ms >= static_cast<std::uint32_t>(g_end * 1000.0f) || state.position_ms + 50 >= state.length_ms;
		if (g_stop || ended || g_writer_failed)
		{
			finish(g_writer_failed ? "Stopped: a frame could not be written." : g_stop ? "Stopped." : "Done.");
		}
	}
}

void capture_draw_tab(const TheaterState& state)
{
	const float length = state.length_ms / 1000.0f;
	if (g_timeframe_for != state.name)
	{
		g_timeframe_for = state.name;
		g_start = 0.0f;
		g_end = length;
	}

	// the campath's first and last nodes are the start and end, each time the path changes (they can
	// still be set by hand afterwards)
	const CameraState camera = camera_state();
	if (camera.points >= 2)
	{
		const std::uint32_t first = camera.point_ms[0], last = camera.point_ms[(camera.points < 64 ? camera.points : 64) - 1];
		static std::uint32_t path_first = ~0u, path_last = ~0u;
		if (first != path_first || last != path_last)
		{
			path_first = first;
			path_last = last;
			g_start = first / 1000.0f;
			g_end = last / 1000.0f;
		}
	}

	const Step step = g_step;
	const bool busy = step != Step::Idle;
	if (ui_section(ICON_CIRCLE, "Record"))
	{
		ImGui::BeginDisabled(busy);
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float now_width = ImGui::CalcTextSize("Now").x + ImGui::GetStyle().FramePadding.x * 2.0f;
		ui_row_label("Start");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - now_width - spacing);
		ImGui::DragFloat("##start", &g_start, 0.05f, 0.0f, length, "%.2f s");
		ImGui::SameLine();
		if (ImGui::Button("Now##start"))
		{
			g_start = state.position_ms / 1000.0f;
		}

		ui_row_label("End");
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - now_width - spacing);
		ImGui::DragFloat("##end", &g_end, 0.05f, 0.0f, length, "%.2f s");
		ImGui::SameLine();
		if (ImGui::Button("Now##end"))
		{
			g_end = state.position_ms / 1000.0f;
		}

		g_start = g_start < 0.0f ? 0.0f : g_start > length ? length : g_start;
		g_end = g_end < 0.0f ? 0.0f : g_end > length ? length : g_end;

		ui_row_label("Output Format");
		if (ImGui::BeginCombo("##format", g_format == Format::Video ? "Video" : "Image Sequence"))
		{
			if (ImGui::Selectable("Video", g_format == Format::Video))
			{
				g_format = Format::Video;
			}

			if (ImGui::Selectable("Image Sequence", g_format == Format::ImageSequence))
			{
				g_format = Format::ImageSequence;
			}

			ImGui::EndCombo();
		}

		if (g_format == Format::Video)
		{
			ui_row_label("Video Codec");
			if (ImGui::BeginCombo("##codec", CODECS[g_codec].label))
			{
				for (int i = 0; i < static_cast<int>(sizeof(CODECS) / sizeof(CODECS[0])); ++i)
				{
					if (ImGui::Selectable(CODECS[i].label, i == g_codec))
					{
						g_codec = i;
					}
				}

				ImGui::EndCombo();
			}
		}

		ui_row_label("Resolution");
		if (ImGui::BeginCombo("##size", SIZES[g_size].label))
		{
			for (int i = 0; i < static_cast<int>(sizeof(SIZES) / sizeof(SIZES[0])); ++i)
			{
				if (ImGui::Selectable(SIZES[i].label, i == g_size))
				{
					g_size = i;
				}
			}

			ImGui::EndCombo();
		}

		char rate[16];
		snprintf(rate, sizeof(rate), "%d", g_rate);
		ui_row_label("Framerate");
		if (ImGui::BeginCombo("##rate", rate))
		{
			for (const int option : RATES)
			{
				snprintf(rate, sizeof(rate), "%d", option);
				if (ImGui::Selectable(rate, option == g_rate))
				{
					g_rate = option;
				}
			}

			ImGui::EndCombo();
		}

		ImGui::EndDisabled();
		ImGui::Spacing();
		const std::string folder = captures_folder();
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextDisabled("%s", folder.c_str());
		ImGui::PopTextWrapPos();
		if (ImGui::Button(ICON_FOLDER_OPEN "  Open the folder"))
		{
			ShellExecuteA(nullptr, "open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}

		ImGui::Spacing();
		const ImVec2 wide = {-FLT_MIN, ui_font_size() * 2.0f};
		if (!busy)
		{
			ImGui::BeginDisabled(g_end <= g_start);
			if (ImGui::Button(ICON_CIRCLE "  Start recording", wide))
			{
				g_message.clear();
				g_waited = 0;
				if (camera.points >= 2)
				{
					// a path there is: it is what is recorded, from wherever the camera was left
					camera_set_mode(CameraMode::Dolly);
				}

				// the passes ticked are recorded one after another (none ticked: what is shown now, once)
				g_passes_left.clear();
				for (int i = 0; i < passes_count(); ++i)
				{
					if (passes_ticked(i))
					{
						g_passes_left.push_back(i);
					}
				}

				g_passes_run = !g_passes_left.empty();
				if (g_passes_run)
				{
					passes_use(g_passes_left.front());
					g_passes_left.erase(g_passes_left.begin());
				}

				theater_set_paused(true);
				theater_seek(static_cast<std::uint32_t>(g_start * 1000.0f));
				g_step = Step::Seeking;
			}

			ImGui::EndDisabled();
		}
		else if (ImGui::Button(ICON_STOP "  Stop recording", wide))
		{
			if (step == Step::Seeking)
			{
				g_step = Step::Idle;
			}
			else
			{
				g_stop = true;
			}
		}

		if (step == Step::Seeking)
		{
			ImGui::TextUnformatted("Going to the start...");
		}
		else if (step == Step::Recording)
		{
			const float total = (g_end - g_start) > 0.0f ? (g_end - g_start) : 1.0f;
			const float done = (state.position_ms / 1000.0f - g_start) / total;
			ImGui::ProgressBar(done < 0.0f ? 0.0f : done > 1.0f ? 1.0f : done, {-FLT_MIN, 0.0f});
			ImGui::Text("Captured %d frames", g_frames.load());
		}

		ImGui::PushTextWrapPos(0.0f);
		if (!g_message.empty())
		{
			ImGui::TextUnformatted(g_message.c_str());
		}

		if (!busy && find_ffmpeg().empty())
		{
			ImGui::TextDisabled("FFmpeg was not found: only an image sequence (TGA pictures) can be recorded. Iris Studio's launcher downloads "
			                    "FFmpeg when it is started.");
		}

		if (camera.points >= 2)
		{
			ImGui::TextDisabled("Start and End follow the campath's first and last nodes; recording switches to the dolly camera.");
		}

		ImGui::TextDisabled("The video is what the picture shows: the camera, the Look tab and the HUD as they are set. The timescale applies, so "
		                    "0.5 gives a video twice as long. No sound is recorded.");
		ImGui::PopTextWrapPos();
	}
}
