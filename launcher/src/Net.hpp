#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// HTTPS through WinHTTP, for what the launcher downloads only with the user's OK (OpenAssetTools) and its update check
namespace Launcher::Net
{
    // A GET of an https URL, following redirects, into memory: none, with why in error, on a failure, a status other
    // than 200, more than maxBytes, or cancel set. progress gets the bytes so far and the total (0 if not known).
    std::optional<std::vector<std::uint8_t>> Get(const std::wstring& url, std::size_t maxBytes,
                                                 const std::atomic<bool>& cancel, std::string& error,
                                                 const std::function<void(std::uint64_t, std::uint64_t)>& progress = {},
                                                 const std::wstring& accept = L"*/*");
}  // namespace Launcher::Net
