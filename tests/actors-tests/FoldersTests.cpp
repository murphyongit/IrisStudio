#include "StdInclude.hpp"
#include "Test.hpp"

#include "Setup/Folders.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

using namespace IWXMVM;

// The folders' move from their names before Iris Studio (Setup/Folders): the paths in
// the mod's JSON files, and the move itself in a folder under %TEMP%, held open once

namespace
{
    const std::filesystem::path OLD_MOD = R"(C:\Users\me\AppData\Local\Plutonium\storage\t5\IWXMVM)";
    const std::filesystem::path NEW_MOD = R"(C:\Users\me\AppData\Local\Plutonium\storage\t5\iris)";
    const std::filesystem::path OLD_HOME = R"(C:\Users\me\AppData\Local\bo1mvm)";
    const std::filesystem::path NEW_HOME = R"(C:\Users\me\AppData\Local\IrisStudio)";
    const std::vector<Setup::FolderRename> RENAMES = {{OLD_MOD, NEW_MOD}, {OLD_HOME, NEW_HOME}};

    // a folder of its own under %TEMP%, gone again at the end of the test
    struct TempFolder
    {
        std::filesystem::path path;

        explicit TempFolder(std::string_view name)
            : path(std::filesystem::temp_directory_path() /
                   std::format("iris-folders-test-{}-{}", GetCurrentProcessId(), name))
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
            std::filesystem::create_directories(path, error);
        }

        ~TempFolder()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

    void Write(const std::filesystem::path& path, std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    std::string Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    }

    // the user's config.json as the mod writes it, with a preset in the mod's folder and one elsewhere
    constexpr std::string_view CONFIG = R"({
    "preferences": {
        "recentVisualPresets": [
            "C:\\Users\\me\\Desktop\\t5mvm\\user_configs\\configs\\me.cfg",
            "C:\\Users\\me\\AppData\\Local\\Plutonium\\storage\\t5\\IWXMVM\\me.cfg"
        ],
        "showActorPaths": true
    }
}
)";
}  // namespace

TEST("the mod looks for FFmpeg in the launcher's folder, then its folder before Iris Studio, then codmvm's")
{
    const auto places = Setup::FFmpegPlaces(R"(C:\L)", R"(C:\R)");
    CHECK(places.size() == 3);
    CHECK(places[0] == std::filesystem::path(R"(C:\L\IrisStudio\ffmpeg.exe)"));
    CHECK(places[1] == std::filesystem::path(R"(C:\L\bo1mvm\ffmpeg.exe)"));
    CHECK(places[2] == std::filesystem::path(R"(C:\R\codmvm_launcher\ffmpeg.exe)"));
}

TEST("a path in the old folder moves to the new one, and every other byte stays as it was")
{
    const auto rewritten = Setup::RewritePaths(CONFIG, RENAMES);
    CHECK(rewritten.has_value());
    if (!rewritten)
        return;
    std::string expected(CONFIG);
    const std::string_view old = R"(storage\\t5\\IWXMVM\\me.cfg)";
    expected.replace(expected.find(old), old.size(), R"(storage\\t5\\iris\\me.cfg)");
    CHECK(*rewritten == expected);
}

TEST("paths are matched in any case and with forward slashes, the folder itself too")
{
    const auto rewritten = Setup::RewritePaths(
        R"({"a": "c:\\users\\ME\\appdata\\local\\bo1mvm\\ffmpeg.exe", "b": "C:/Users/me/AppData/Local/bo1mvm/tools",)"
        R"( "c": "C:\\Users\\me\\AppData\\Local\\bo1mvm"})",
        RENAMES);
    CHECK(rewritten.has_value());
    if (rewritten)
        CHECK(*rewritten == R"({"a": "C:\\Users\\me\\AppData\\Local\\IrisStudio\\ffmpeg.exe", )"
                            R"("b": "C:/Users/me/AppData/Local/IrisStudio/tools", )"
                            R"("c": "C:\\Users\\me\\AppData\\Local\\IrisStudio"})");
}

TEST("only paths that start a string and name the whole folder are moved")
{
    // a longer folder name, the path in the middle of a string, a key of that name, a relative path
    for (const std::string_view text :
         {R"({"a": "C:\\Users\\me\\AppData\\Local\\bo1mvm2\\x"})",
          R"({"a": "see C:\\Users\\me\\AppData\\Local\\bo1mvm\\x"})", R"({"IWXMVM": "IWXMVM\\zones"})",
          R"({"a": "storage\\t5\\IWXMVM\\me.cfg"})", R"({"a": "\"C:\\Users\\me\\AppData\\Local\\bo1mvm\\x"})"})
        CHECK(!Setup::RewritePaths(text, RENAMES).has_value());
}

TEST("the old folder is renamed to the new one, and only its .json files with old paths are written")
{
    TempFolder temp("move");
    const auto from = temp.path / "IWXMVM", to = temp.path / "iris";
    const std::string config =
        std::format(R"({{"recentVisualPresets": ["{}"], "x": 1}})", (from / "me.cfg").string());
    std::string escaped;
    for (const char c : config)
        escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
    Write(from / "config.json", escaped);
    Write(from / "scenes" / "A.json", R"({"actors": []})");
    Write(from / "me.cfg", std::string("bind F \"x\"\r\n") + (from / "x").string());
    Write(from / "zones" / "z.ff", std::string("\0\1\2", 3));

    const std::vector<Setup::FolderRename> renames = {{from, to}};
    const auto move = Setup::MoveFolder(renames[0], renames);
    CHECK(move.moved);
    CHECK(move.problem.empty());
    CHECK(move.folder == to);
    CHECK(!std::filesystem::exists(from));
    CHECK(move.rewritten.size() == 1 && move.rewritten[0] == to / "config.json");

    std::string expected;
    for (const char c : std::format(R"({{"recentVisualPresets": ["{}"], "x": 1}})", (to / "me.cfg").string()))
        expected += c == '\\' ? std::string("\\\\") : std::string(1, c);
    CHECK(Read(to / "config.json") == expected);
    CHECK(Read(to / "scenes" / "A.json") == R"({"actors": []})");
    // a file that isn't JSON keeps even a path in it
    CHECK(Read(to / "me.cfg") == std::string("bind F \"x\"\r\n") + (from / "x").string());
    CHECK(Read(to / "zones" / "z.ff") == std::string("\0\1\2", 3));
    CHECK(!std::filesystem::exists(to / "config.json.part"));
}

TEST("a new folder that is there already stays the one, and the old one is left alone")
{
    TempFolder temp("both");
    const auto from = temp.path / "bo1mvm", to = temp.path / "IrisStudio";
    Write(from / "launcher.json", "{}");
    Write(to / "launcher.json", R"({"palette": "mono"})");
    const std::vector<Setup::FolderRename> renames = {{from, to}};
    const auto move = Setup::MoveFolder(renames[0], renames);
    CHECK(!move.moved);
    CHECK(move.folder == to);
    CHECK(Read(from / "launcher.json") == "{}");
    CHECK(Read(to / "launcher.json") == R"({"palette": "mono"})");

    // and with neither, the new one is where files will go
    const std::vector<Setup::FolderRename> none = {{temp.path / "a", temp.path / "b"}};
    const auto empty = Setup::MoveFolder(none[0], none);
    CHECK(!empty.moved && empty.problem.empty() && empty.folder == temp.path / "b");
    CHECK(!std::filesystem::exists(temp.path / "b"));
}

TEST("while a file in it is open the old folder stays the one, whole, and the problem says why")
{
    TempFolder temp("held");
    const auto from = temp.path / "IWXMVM", to = temp.path / "iris";
    Write(from / "config.json", "{}");
    Write(from / "zones" / "z.ff", "zone");
    // held the way the game holds a zone it reads: shared for reading, not for deleting
    const HANDLE held = CreateFileW((from / "zones" / "z.ff").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    const std::vector<Setup::FolderRename> renames = {{from, to}};
    const auto move = Setup::MoveFolder(renames[0], renames);
    CloseHandle(held);
    CHECK(!move.moved);
    CHECK(move.folder == from);
    CHECK(!move.problem.empty());
    CHECK(Read(from / "config.json") == "{}" && Read(from / "zones" / "z.ff") == "zone");
    CHECK(!std::filesystem::exists(to));

    // once it's closed, the next start moves it
    const auto later = Setup::MoveFolder(renames[0], renames);
    CHECK(later.moved && later.folder == to && Read(to / "zones" / "z.ff") == "zone");
}
