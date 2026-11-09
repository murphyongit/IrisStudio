#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The folders the mod and the launcher keep their files in, and the move from their names before Iris Studio
// (2026-10-28): the mod's folder in the game's user folder (Plutonium's storage\t5) was
// IWXMVM and is iris, the launcher's in %LOCALAPPDATA% was bo1mvm and is IrisStudio. Both the mod and the launcher
// move a folder of the old name the first time they find one, so either can come first.
namespace IWXMVM::Setup
{
    inline constexpr std::string_view MOD_FOLDER = "iris";
    inline constexpr std::string_view OLD_MOD_FOLDER = "IWXMVM";
    inline constexpr std::string_view LAUNCHER_FOLDER = "IrisStudio";
    inline constexpr std::string_view OLD_LAUNCHER_FOLDER = "bo1mvm";
    // the Black Ops mod's DLL, which was t5.dll
    inline constexpr std::string_view T5_DLL = "T5iris.dll";
    inline constexpr std::string_view OLD_T5_DLL = "t5.dll";

    // where the mod looks for FFmpeg, first one first: the launcher's folder, its folder before Iris Studio, then the
    // codmvm launcher's
    std::vector<std::filesystem::path> FFmpegPlaces(const std::filesystem::path& localAppData,
                                                    const std::filesystem::path& roamingAppData);

    struct FolderRename
    {
        std::filesystem::path from;
        std::filesystem::path to;
    };

    // `text` (a JSON file) with every path under one of the renames' `from` folders moved under its `to`, and every
    // other byte as it was; none when it has no such path. A path counts when a string starts with it, written as JSON
    // writes it (\\ or /), in any case, and the folder's name ends there.
    std::optional<std::string> RewritePaths(std::string_view text, std::span<const FolderRename> renames);

    struct FolderMove
    {
        std::filesystem::path folder;  // the one to use: `to`, or `from` while it couldn't be moved
        bool moved = false;            // `from` became `to` just now
        std::string problem;           // why `from` is still the one, or which files kept their old paths
        std::vector<std::filesystem::path> rewritten;  // the .json files whose paths were rewritten
    };

    // Renames `rename.from` to `rename.to` when `from` is there and `to` isn't, in one move on its volume (never a
    // copy), then rewrites the paths of `renames` in the .json files in it. When `to` is there already it stays the
    // folder and `from` is left alone; when the move fails (a file in it is open), `from` stays the folder.
    FolderMove MoveFolder(const FolderRename& rename, std::span<const FolderRename> renames);
}  // namespace IWXMVM::Setup
