#include "StdInclude.hpp"
#include "Folders.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

namespace IWXMVM::Setup
{
    namespace
    {
        // the largest .json file whose paths are rewritten; the mod's own are a few hundred KB at most
        constexpr std::uintmax_t MAX_REWRITTEN_SIZE = 16 * 1024 * 1024;

        std::string Utf8(const std::filesystem::path& path)
        {
            const auto text = path.u8string();
            return std::string(reinterpret_cast<const char*>(text.data()), text.size());
        }

        // a folder's path as JSON has it: each separator an escaped backslash, or a forward slash
        std::string Written(std::string_view path, bool escaped)
        {
            std::string written;
            for (const char c : path)
            {
                if (c == '\\' || c == '/')
                    written += escaped ? "\\\\" : "/";
                else
                    written += c;
            }
            return written;
        }

        // ASCII letters in any case, other bytes as they are
        bool StartsWithIgnoringCase(std::string_view text, std::string_view prefix)
        {
            return text.size() >= prefix.size() &&
                   std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                   });
        }

        std::string WithoutTrailingSeparators(std::string path)
        {
            while (path.size() > 1 && (path.back() == '\\' || path.back() == '/'))
                path.pop_back();
            return path;
        }

        bool IsJson(const std::filesystem::path& path)
        {
            auto extension = path.extension().string();
            for (auto& c : extension)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return extension == ".json";
        }

        std::optional<std::string> ReadFile(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
                return std::nullopt;
            std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (stream.bad())
                return std::nullopt;
            return text;
        }

        // written next to the file, then put in its place in one step, so a failure leaves the old file whole
        bool ReplaceFile(const std::filesystem::path& path, std::string_view text)
        {
            auto part = path;
            part += ".part";
            {
                std::ofstream stream(part, std::ios::binary | std::ios::trunc);
                stream.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!stream)
                {
                    stream.close();
                    std::error_code error;
                    std::filesystem::remove(part, error);
                    return false;
                }
            }
            if (MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                return true;
            std::error_code error;
            std::filesystem::remove(part, error);
            return false;
        }
    }  // namespace

    std::vector<std::filesystem::path> FFmpegPlaces(const std::filesystem::path& localAppData,
                                                    const std::filesystem::path& roamingAppData)
    {
        return {localAppData / std::filesystem::path(LAUNCHER_FOLDER) / "ffmpeg.exe",
                localAppData / std::filesystem::path(OLD_LAUNCHER_FOLDER) / "ffmpeg.exe",
                roamingAppData / "codmvm_launcher" / "ffmpeg.exe"};
    }

    std::optional<std::string> RewritePaths(std::string_view text, std::span<const FolderRename> renames)
    {
        struct Needle
        {
            std::string from;
            std::string to;
        };
        std::vector<Needle> needles;
        for (const auto& rename : renames)
        {
            const auto from = WithoutTrailingSeparators(Utf8(rename.from));
            const auto to = WithoutTrailingSeparators(Utf8(rename.to));
            if (from.empty() || to.empty())
                continue;
            for (const bool escaped : {true, false})
                needles.push_back({Written(from, escaped), Written(to, escaped)});
        }

        std::string rewritten;
        std::size_t copied = 0;
        // a string's first character: after a quote that isn't an escaped one
        for (std::size_t i = 1; i < text.size(); ++i)
        {
            if (text[i - 1] != '"' || (i >= 2 && text[i - 2] == '\\'))
                continue;
            for (const auto& needle : needles)
            {
                const auto end = i + needle.from.size();
                // the folder's whole name: the string or the path goes on with a separator after it
                if (end >= text.size() || (text[end] != '"' && text[end] != '\\' && text[end] != '/') ||
                    !StartsWithIgnoringCase(text.substr(i), needle.from))
                    continue;
                rewritten.append(text.substr(copied, i - copied));
                rewritten += needle.to;
                copied = end;
                i = end - 1;
                break;
            }
        }
        if (copied == 0)
            return std::nullopt;
        rewritten.append(text.substr(copied));
        return rewritten;
    }

    FolderMove MoveFolder(const FolderRename& rename, std::span<const FolderRename> renames)
    {
        FolderMove result{rename.to};
        std::error_code error;
        if (std::filesystem::exists(rename.to, error) || !std::filesystem::is_directory(rename.from, error))
            return result;

        // without MOVEFILE_COPY_ALLOWED: a rename on the folder's volume, or nothing
        if (!MoveFileExW(rename.from.c_str(), rename.to.c_str(), 0))
        {
            const auto code = static_cast<int>(GetLastError());
            result.folder = rename.from;
            result.problem = std::format("{} couldn't be renamed to {}: {}", Utf8(rename.from),
                                         Utf8(rename.to.filename()), std::system_category().message(code));
            return result;
        }
        result.moved = true;

        std::vector<std::string> kept;
        for (auto it = std::filesystem::recursive_directory_iterator(rename.to, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
        {
            const auto& path = it->path();
            std::error_code sizeError;
            if (!it->is_regular_file(sizeError) || !IsJson(path) || it->file_size(sizeError) > MAX_REWRITTEN_SIZE)
                continue;
            const auto text = ReadFile(path);
            if (!text)
            {
                kept.push_back(Utf8(path.filename()));
                continue;
            }
            const auto changed = RewritePaths(*text, renames);
            if (!changed)
                continue;
            if (ReplaceFile(path, *changed))
                result.rewritten.push_back(path);
            else
                kept.push_back(Utf8(path.filename()));
        }
        if (!kept.empty())
        {
            result.problem = "kept their old paths:";
            for (const auto& name : kept)
                result.problem += " " + name;
        }
        return result;
    }
}  // namespace IWXMVM::Setup
