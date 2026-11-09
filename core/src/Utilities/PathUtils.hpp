#pragma once

namespace IWXMVM::PathUtils
{
    std::string GetCurrentExecutablePath();
    std::string GetCurrentGameDirectory();
    // The mod's folder in the game's user folder: iris, moved there from IWXMVM (its name before Iris Studio) the
    // first time it is asked for; IWXMVM while that folder can't be moved (Setup/Folders.hpp)
    std::filesystem::path GetModPath();
    std::optional<std::filesystem::path> OpenFileDialog(bool saveDialog, DWORD flags, const char* filterString,
                                                        const char* extension, 
                                                        std::optional<std::filesystem::path> initialDir = std::nullopt);
    std::optional<std::filesystem::path> OpenFolderBrowseDialog();
}  // namespace IWXMVM::PathUtils