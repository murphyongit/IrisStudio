#include "Platform.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include <cstring>
#include <format>
#include <fstream>
#include <vector>

namespace Launcher::Platform
{
    namespace
    {
        std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
        {
            PWSTR path = nullptr;
            std::filesystem::path result;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
                result = path;
            CoTaskMemFree(path);
            return result;
        }

        template <std::size_t N>
        std::array<std::uint8_t, N> Digest(const wchar_t* algorithm, std::span<const std::uint8_t> bytes)
        {
            std::array<std::uint8_t, N> digest{};
            BCRYPT_ALG_HANDLE provider = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            if (BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr, 0) == 0 &&
                BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) == 0)
            {
                // in pieces, since BCryptHashData takes a ULONG
                constexpr std::size_t PIECE = 1u << 30;
                for (std::size_t at = 0; at < bytes.size(); at += PIECE)
                {
                    const auto size = std::min(PIECE, bytes.size() - at);
                    BCryptHashData(hash, const_cast<PUCHAR>(bytes.data() + at), static_cast<ULONG>(size), 0);
                }
                BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
            }
            if (hash)
                BCryptDestroyHash(hash);
            if (provider)
                BCryptCloseAlgorithmProvider(provider, 0);
            return digest;
        }

        HWND OwnerWindow()
        {
            return FindWindowW(L"IrisStudio", nullptr);
        }
    }  // namespace

    std::span<const std::uint8_t> Resource(int id)
    {
        const auto info = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
        if (!info)
            return {};
        const auto handle = LoadResource(nullptr, info);
        if (!handle)
            return {};
        return {static_cast<const std::uint8_t*>(LockResource(handle)), SizeofResource(nullptr, info)};
    }

    std::string_view ResourceText(int id)
    {
        const auto bytes = Resource(id);
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    std::filesystem::path LocalAppData()
    {
        return KnownFolder(FOLDERID_LocalAppData);
    }

    std::filesystem::path RoamingAppData()
    {
        return KnownFolder(FOLDERID_RoamingAppData);
    }

    std::optional<std::wstring> RegistryString(bool currentUser, const wchar_t* key, const wchar_t* value,
                                               unsigned long view)
    {
        DWORD size = 0;
        const auto root = currentUser ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | (view == KEY_WOW64_64KEY   ? RRF_SUBKEY_WOW6464KEY
                                                                    : view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY
                                                                                              : 0);
        if (RegGetValueW(root, key, value, flags, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0)
            return std::nullopt;
        std::wstring text(size / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, key, value, flags, nullptr, text.data(), &size) != ERROR_SUCCESS)
            return std::nullopt;
        text.resize(wcsnlen(text.c_str(), text.size()));
        return text;
    }

    std::optional<std::string> ReadFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;
        std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (file.bad())
            return std::nullopt;
        return text;
    }

    bool WriteFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        auto temporary = path;
        temporary += ".part";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                return false;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
        return true;
    }

    bool WriteFile(const std::filesystem::path& path, std::string_view text)
    {
        return WriteFile(path, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    std::optional<std::array<std::uint8_t, 32>> Sha256File(const std::filesystem::path& path)
    {
        const auto bytes = ReadFile(path);
        if (!bytes)
            return std::nullopt;
        return Sha256(std::span(reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size()));
    }

    std::array<std::uint8_t, 32> Sha256(std::span<const std::uint8_t> bytes)
    {
        return Digest<32>(BCRYPT_SHA256_ALGORITHM, bytes);
    }

    std::array<std::uint8_t, 20> Sha1(std::span<const std::uint8_t> bytes)
    {
        return Digest<20>(BCRYPT_SHA1_ALGORITHM, bytes);
    }

    std::string Hex(std::span<const std::uint8_t> bytes)
    {
        std::string text;
        for (const auto byte : bytes)
            text += std::format("{:02x}", byte);
        return text;
    }

    std::uint64_t FreeSpace(const std::filesystem::path& path)
    {
        auto existing = path;
        std::error_code error;
        while (!existing.empty() && !std::filesystem::is_directory(existing, error) && existing.has_parent_path() &&
               existing.parent_path() != existing)
            existing = existing.parent_path();
        ULARGE_INTEGER free{};
        if (!GetDiskFreeSpaceExW(existing.c_str(), &free, nullptr, nullptr))
            return 0;
        return free.QuadPart;
    }

    void OpenUrl(const wchar_t* url)
    {
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenFolder(const std::filesystem::path& folder)
    {
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        ShellExecuteW(nullptr, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void OpenFile(const std::filesystem::path& file)
    {
        ShellExecuteW(nullptr, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    std::optional<std::string> ClipboardText()
    {
        if (!OpenClipboard(OwnerWindow()))
            return std::nullopt;
        std::optional<std::string> text;
        if (const HANDLE data = GetClipboardData(CF_UNICODETEXT))
        {
            if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(data)))
            {
                text = Utf8(std::filesystem::path(wide));
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return text;
    }

    bool SetClipboardText(std::string_view text)
    {
        const auto wide = FromUtf8(text).wstring();
        const HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
        if (!data)
            return false;
        std::memcpy(GlobalLock(data), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(data);
        if (!OpenClipboard(OwnerWindow()))
        {
            GlobalFree(data);
            return false;
        }
        EmptyClipboard();
        // the clipboard owns the memory once it took it
        const bool set = SetClipboardData(CF_UNICODETEXT, data) != nullptr;
        if (!set)
            GlobalFree(data);
        CloseClipboard();
        return set;
    }

    namespace
    {
        std::optional<std::filesystem::path> Pick(bool folder, const wchar_t* title, const std::filesystem::path& start,
                                                  const wchar_t* filterName, const wchar_t* filter)
        {
            IFileOpenDialog* dialog = nullptr;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
                return std::nullopt;
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : 0));
            dialog->SetTitle(title);
            if (filter)
            {
                const COMDLG_FILTERSPEC spec{filterName, filter};
                dialog->SetFileTypes(1, &spec);
            }
            std::error_code error;
            if (!start.empty() && std::filesystem::is_directory(start, error))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item))))
                {
                    dialog->SetFolder(item);
                    item->Release();
                }
            }
            std::optional<std::filesystem::path> picked;
            if (SUCCEEDED(dialog->Show(OwnerWindow())))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                        picked = std::filesystem::path(path);
                    CoTaskMemFree(path);
                    item->Release();
                }
            }
            dialog->Release();
            return picked;
        }
    }  // namespace

    std::optional<std::filesystem::path> PickFolder(const wchar_t* title, const std::filesystem::path& start)
    {
        return Pick(true, title, start, nullptr, nullptr);
    }

    std::optional<std::filesystem::path> PickFile(const wchar_t* title, const wchar_t* filterName,
                                                  const wchar_t* filter)
    {
        return Pick(false, title, {}, filterName, filter);
    }

    std::string Utf8(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return {text.begin(), text.end()};
    }

    std::filesystem::path FromUtf8(std::string_view text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
}  // namespace Launcher::Platform
