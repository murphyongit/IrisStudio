#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Windows' part of the launcher: its embedded files, known folders, the registry, hashes, dialogs and the shell
namespace Launcher::Platform
{
    // a file launcher.rc embeds (res/resource.h)
    std::span<const std::uint8_t> Resource(int id);
    std::string_view ResourceText(int id);

    std::filesystem::path LocalAppData();
    std::filesystem::path RoamingAppData();

    // a string value of the registry; view is KEY_WOW64_64KEY, KEY_WOW64_32KEY or 0
    std::optional<std::wstring> RegistryString(bool currentUser, const wchar_t* key, const wchar_t* value,
                                               unsigned long view = 0);

    // a whole file; none if it can't be read
    std::optional<std::string> ReadFile(const std::filesystem::path& path);
    // writes a file through a temporary one next to it, so it is never half written
    bool WriteFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);
    bool WriteFile(const std::filesystem::path& path, std::string_view text);

    // SHA-256 and SHA-1 (Windows' CNG); none if the file can't be read
    std::optional<std::array<std::uint8_t, 32>> Sha256File(const std::filesystem::path& path);
    std::array<std::uint8_t, 32> Sha256(std::span<const std::uint8_t> bytes);
    std::array<std::uint8_t, 20> Sha1(std::span<const std::uint8_t> bytes);
    std::string Hex(std::span<const std::uint8_t> bytes);

    // the free space of the drive a path is on (or would be, for one that doesn't exist yet)
    std::uint64_t FreeSpace(const std::filesystem::path& path);

    void OpenUrl(const wchar_t* url);
    void OpenFolder(const std::filesystem::path& folder);
    void OpenFile(const std::filesystem::path& file);

    // the clipboard's text as UTF-8 (none when it has none), and text put on it
    std::optional<std::string> ClipboardText();
    bool SetClipboardText(std::string_view text);

    // the dialogs to pick a folder or a file, owned by the launcher's window; none when cancelled
    std::optional<std::filesystem::path> PickFolder(const wchar_t* title, const std::filesystem::path& start);
    std::optional<std::filesystem::path> PickFile(const wchar_t* title, const wchar_t* filterName,
                                                  const wchar_t* filter);

    // a path as UTF-8, as the UI and JSON have text
    std::string Utf8(const std::filesystem::path& path);
    std::filesystem::path FromUtf8(std::string_view text);
}  // namespace Launcher::Platform
