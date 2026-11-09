#include "Net.hpp"

#include <Windows.h>
#include <winhttp.h>

#include <format>

#include "Version.hpp"

namespace Launcher::Net
{
    namespace
    {
        struct Handle
        {
            HINTERNET handle = nullptr;
            ~Handle()
            {
                if (handle)
                    WinHttpCloseHandle(handle);
            }
        };
    }  // namespace

    std::optional<std::vector<std::uint8_t>> Get(const std::wstring& url, std::size_t maxBytes,
                                                 const std::atomic<bool>& cancel, std::string& error,
                                                 const std::function<void(std::uint64_t, std::uint64_t)>& progress,
                                                 const std::wstring& accept)
    {
        URL_COMPONENTS parts{sizeof(parts)};
        wchar_t host[256]{}, path[2048]{};
        parts.lpszHostName = host;
        parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        {
            error = "not an https address";
            return std::nullopt;
        }

        Handle session{WinHttpOpen(L"IrisStudio/" IRIS_STUDIO_VERSION_STRING_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.handle)
            session.handle = WinHttpOpen(L"IrisStudio/" IRIS_STUDIO_VERSION_STRING_W, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        Handle connection{session.handle ? WinHttpConnect(session.handle, host, parts.nPort, 0) : nullptr};
        Handle request{connection.handle ? WinHttpOpenRequest(connection.handle, L"GET", path, nullptr,
                                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                              WINHTTP_FLAG_SECURE)
                                         : nullptr};
        if (!request.handle)
        {
            error = std::format("couldn't connect (error {})", GetLastError());
            return std::nullopt;
        }
        WinHttpSetTimeouts(request.handle, 10000, 10000, 15000, 30000);
        const auto headers = L"Accept: " + accept;
        if (!WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0,
                                0) ||
            !WinHttpReceiveResponse(request.handle, nullptr))
        {
            error = std::format("the download failed (error {})", GetLastError());
            return std::nullopt;
        }

        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200)
        {
            error = std::format("the server answered {}", status);
            return std::nullopt;
        }
        std::uint64_t total = 0;
        wchar_t lengthText[32]{};
        size = sizeof(lengthText);
        if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
                                &size, WINHTTP_NO_HEADER_INDEX))
            total = std::wcstoull(lengthText, nullptr, 10);
        if (total > maxBytes)
        {
            error = "the file is bigger than expected";
            return std::nullopt;
        }

        std::vector<std::uint8_t> body;
        body.reserve(static_cast<std::size_t>(total));
        while (true)
        {
            if (cancel)
            {
                error = "cancelled";
                return std::nullopt;
            }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.handle, &available))
            {
                error = std::format("the download stopped (error {})", GetLastError());
                return std::nullopt;
            }
            if (available == 0)
                break;
            const auto at = body.size();
            if (at + available > maxBytes)
            {
                error = "the file is bigger than expected";
                return std::nullopt;
            }
            body.resize(at + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.handle, body.data() + at, available, &read))
            {
                error = std::format("the download stopped (error {})", GetLastError());
                return std::nullopt;
            }
            body.resize(at + read);
            if (progress)
                progress(body.size(), total);
        }
        return body;
    }
}  // namespace Launcher::Net
