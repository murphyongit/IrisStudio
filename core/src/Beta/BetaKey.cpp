#include "StdInclude.hpp"
#include "BetaKey.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

#include "Setup/Folders.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace IWXMVM::Beta
{
    namespace
    {
        // Crockford's base32, without I, L, O and U
        constexpr std::string_view ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
        constexpr std::string_view PREFIX = "IRIS-";
        // what is signed starts with this, so a signature of anything else is never a key's
        constexpr std::string_view SIGNED_PREFIX = "IrisStudio beta key\n";
        constexpr std::string_view PC_PREFIX = "IrisStudio PC 1|";
        constexpr std::uint8_t FORMAT = 1;
        // the format's version, the name's length, the PC code (5 bytes), the end day and the number (2 bytes each)
        constexpr std::size_t HEADER_SIZE = 11;
        constexpr std::size_t SIGNATURE_SIZE = 64;
        constexpr std::size_t MAX_NAME = 32;
        constexpr std::uintmax_t MAX_KEY_FILE = 4096;
        constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

        std::optional<Key> active;

        std::span<const std::uint8_t> Bytes(std::string_view text)
        {
            return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
        }

        std::array<std::uint8_t, 32> Sha256(std::span<const std::uint8_t> bytes)
        {
            std::array<std::uint8_t, 32> digest{};
            BCRYPT_ALG_HANDLE provider = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&provider, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            {
                if (BCRYPT_SUCCESS(BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0)))
                {
                    BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0);
                    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
                    BCryptDestroyHash(hash);
                }
                BCryptCloseAlgorithmProvider(provider, 0);
            }
            return digest;
        }

        // the first `count` bits of `bytes`, the first one highest
        std::uint64_t Bits(std::span<const std::uint8_t> bytes, int count)
        {
            std::uint64_t value = 0;
            for (int i = 0; i < count; ++i)
                value = (value << 1) | ((bytes[i / 8] >> (7 - i % 8)) & 1);
            return value;
        }

        // the 8th character of a PC code: the first 5 bits of SHA-256 of the 7 before it
        char CheckCharacter(std::string_view seven)
        {
            return ALPHABET[static_cast<std::size_t>(Bits(Sha256(Bytes(seven)), 5))];
        }

        int Base64UrlValue(char c)
        {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '-')
                return 62;
            if (c == '_')
                return 63;
            return -1;
        }

        // base64url characters without padding; the bits of a last, partial byte are dropped
        std::vector<std::uint8_t> FromBase64Url(std::string_view chars)
        {
            std::vector<std::uint8_t> bytes;
            std::uint32_t buffer = 0;
            int bits = 0;
            for (const char c : chars)
            {
                buffer = (buffer << 6) | static_cast<std::uint32_t>(Base64UrlValue(c));
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    bytes.push_back(static_cast<std::uint8_t>(buffer >> bits));
                }
            }
            return bytes;
        }

        bool IsSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        // a key right after its prefix: its header's first 3 bytes give its length, then that many characters are read
        std::optional<Key> ReadKey(std::string_view rest)
        {
            std::string chars;
            std::size_t needed = 4;
            std::size_t nameSize = 0;
            for (const char c : rest)
            {
                if (chars.size() == needed)
                    break;
                if (IsSpace(c))
                    continue;
                if (Base64UrlValue(c) < 0)
                    break;
                chars += c;
                if (chars.size() == 4)
                {
                    const auto head = FromBase64Url(chars);
                    nameSize = head[1];
                    if (head[0] != FORMAT || nameSize < 1 || nameSize > MAX_NAME)
                        return std::nullopt;
                    needed = ((HEADER_SIZE + nameSize + SIGNATURE_SIZE) * 8 + 5) / 6;
                }
            }
            if (nameSize == 0 || chars.size() != needed)
                return std::nullopt;
            // the bits of the last character past the last byte are zeros, so each key has one text only
            if (const int spare = static_cast<int>(needed * 6 % 8);
                spare != 0 && (Base64UrlValue(chars.back()) & ((1 << spare) - 1)) != 0)
                return std::nullopt;

            const auto bytes = FromBase64Url(chars);
            const auto signedSize = HEADER_SIZE + nameSize;
            if (bytes.size() != signedSize + SIGNATURE_SIZE)
                return std::nullopt;

            Key key;
            key.text = std::string(PREFIX) + chars;
            const std::uint64_t pc = Bits(std::span(bytes).subspan(2, 5), 40);
            for (int i = 7; i >= 0; --i)
                key.pcCode += ALPHABET[static_cast<std::size_t>((pc >> (5 * i)) & 31)];
            key.endDay = bytes[7] | (bytes[8] << 8);
            key.number = bytes[9] | (bytes[10] << 8);
            key.name.assign(reinterpret_cast<const char*>(bytes.data() + HEADER_SIZE), nameSize);
            key.signedBytes.assign(SIGNED_PREFIX.begin(), SIGNED_PREFIX.end());
            key.signedBytes.insert(key.signedBytes.end(), bytes.begin(), bytes.begin() + signedSize);
            std::copy(bytes.begin() + signedSize, bytes.end(), key.signature.begin());
            return key;
        }
    }  // namespace

    std::string PcCodeOf(std::string_view machineGuid)
    {
        std::string text(PC_PREFIX);
        for (const char c : machineGuid)
            text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto value = Bits(Sha256(Bytes(text)), 35);
        std::string code;
        for (int i = 6; i >= 0; --i)
            code += ALPHABET[static_cast<std::size_t>((value >> (5 * i)) & 31)];
        return code + CheckCharacter(code);
    }

    std::string FormatPcCode(std::string_view code)
    {
        if (code.size() != 8)
            return std::string(code);
        return std::format("{}-{}", code.substr(0, 4), code.substr(4));
    }

    std::optional<std::string> ReadPcCode(std::string_view text)
    {
        std::string code;
        for (const char c : text)
        {
            if (IsSpace(c) || c == '-')
                continue;
            char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (upper == 'O')
                upper = '0';
            else if (upper == 'I' || upper == 'L')
                upper = '1';
            if (ALPHABET.find(upper) == std::string_view::npos)
                return std::nullopt;
            code += upper;
        }
        if (code.size() != 8 || CheckCharacter(std::string_view(code).substr(0, 7)) != code[7])
            return std::nullopt;
        return code;
    }

    int Day(int y, int m, int d)
    {
        using namespace std::chrono;
        const sys_days date = year{y} / static_cast<int>(m) / static_cast<int>(d);
        const sys_days first = year{2026} / January / 1;
        return static_cast<int>((date - first).count());
    }

    std::string DateText(int dayNumber)
    {
        using namespace std::chrono;
        const year_month_day date{sys_days{year{2026} / January / 1} + days{dayNumber}};
        return std::format("{} {} {}", static_cast<unsigned>(date.day()),
                           MONTHS[static_cast<unsigned>(date.month()) - 1], static_cast<int>(date.year()));
    }

    std::optional<Key> FindKey(std::string_view text)
    {
        for (auto at = text.find(PREFIX); at != std::string_view::npos; at = text.find(PREFIX, at + 1))
        {
            if (auto key = ReadKey(text.substr(at + PREFIX.size())))
                return key;
        }
        return std::nullopt;
    }

    bool HasKeyPrefix(std::string_view text)
    {
        return text.find(PREFIX) != std::string_view::npos;
    }

    bool VerifySignature(const PublicKey& publicKey, std::span<const std::uint8_t> message,
                         std::span<const std::uint8_t, 64> signature)
    {
        // the public key as CNG takes it: its header, then X and Y
        const BCRYPT_ECCKEY_BLOB header{BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
        std::array<std::uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + std::tuple_size_v<PublicKey>> blob{};
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), publicKey.data(), publicKey.size());
        auto digest = Sha256(message);

        bool good = false;
        BCRYPT_ALG_HANDLE provider = nullptr;
        BCRYPT_KEY_HANDLE key = nullptr;
        if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&provider, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0)))
        {
            if (BCRYPT_SUCCESS(BCryptImportKeyPair(provider, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(),
                                                   static_cast<ULONG>(blob.size()), 0)))
            {
                good = BCRYPT_SUCCESS(BCryptVerifySignature(
                    key, nullptr, digest.data(), static_cast<ULONG>(digest.size()),
                    const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0));
                BCryptDestroyKey(key);
            }
            BCryptCloseAlgorithmProvider(provider, 0);
        }
        return good;
    }

    Verdict Check(std::string_view text, std::string_view thisPc, int today, const PublicKey& publicKey, Key* found)
    {
        if (!HasKeyPrefix(text))
            return Verdict::NoKey;
        const auto key = FindKey(text);
        if (!key || !VerifySignature(publicKey, key->signedBytes, key->signature))
            return Verdict::NotValid;
        if (found)
            *found = *key;
        if (thisPc.empty())
            return Verdict::NoPcCode;
        if (key->pcCode != thisPc)
            return Verdict::OtherPc;
        if (today > key->endDay)
            return Verdict::Ended;
        return Verdict::Valid;
    }

    std::string KeyScreenLine(Verdict verdict, const Key& key)
    {
        switch (verdict)
        {
            case Verdict::NotValid:
                return "This key isn't valid";
            case Verdict::OtherPc:
                return "This key is for another PC";
            case Verdict::Ended:
                return std::format("This beta ended on {}", DateText(key.endDay));
            case Verdict::NoPcCode:
                return "This PC's code couldn't be read";
            default:
                return "Send this code to get a beta key";
        }
    }

    std::string ValidLine(const Key& key)
    {
        return std::format("Beta key for {}, until {}", key.name, DateText(key.endDay));
    }

    std::string KeyText(const Key& key)
    {
        return std::format("{}, until {}", key.name, DateText(key.endDay));
    }

    std::string AboutLine(const Key& key)
    {
        return std::format("Beta: {}, until {}{}", key.name, DateText(key.endDay), key.testKey ? " (test key)" : "");
    }

    std::string ModMessage(Verdict verdict, std::string_view modName, const Key& key, std::string_view thisPc)
    {
        const auto code = FormatPcCode(thisPc);
        switch (verdict)
        {
            case Verdict::NoKey:
                return std::format(
                    "{} is in beta and needs a key for this PC. Send this PC's code, {}, to get one, "
                    "then paste it in Iris Studio.",
                    modName, code);
            case Verdict::OtherPc:
                return std::format(
                    "This beta key is for another PC. Send this PC's code, {}, to get a key for it, "
                    "then paste it in Iris Studio.",
                    code);
            case Verdict::Ended:
                return std::format("This {} beta ended on {}. Thanks for testing!", modName, DateText(key.endDay));
            case Verdict::NotValid:
                return "This beta key isn't valid. Open Iris Studio and paste it again.";
            case Verdict::NoPcCode:
                return std::format("This PC's code couldn't be read, so {} can't check its beta key.", modName);
            default:
                return "";
        }
    }

    std::vector<std::filesystem::path> KeyFilePlaces(const std::filesystem::path& dllFolder,
                                                     const std::filesystem::path& localAppData)
    {
        std::vector<std::filesystem::path> places;
        if (!dllFolder.empty())
            places.push_back(dllFolder / KEY_FILE);
        if (!localAppData.empty())
        {
            const auto launcher = localAppData / std::filesystem::path(Setup::LAUNCHER_FOLDER) / KEY_FILE;
            if (places.empty() || places.front().lexically_normal() != launcher.lexically_normal())
                places.push_back(launcher);
        }
        return places;
    }

    std::string ReadKeyFile(const std::vector<std::filesystem::path>& places)
    {
        for (const auto& place : places)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(place, error);
            if (error || size > MAX_KEY_FILE)
                continue;
            std::ifstream stream(place, std::ios::binary);
            if (!stream)
                continue;
            return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        }
        return "";
    }

    std::optional<std::string> ThisPcCode()
    {
        // KEY_WOW64_64KEY: a 32-bit program's own view (WOW6432Node) has no MachineGuid
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
                          KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
            return std::nullopt;
        wchar_t value[128]{};
        DWORD size = sizeof(value) - sizeof(wchar_t);
        DWORD type = 0;
        const auto status =
            RegQueryValueExW(key, L"MachineGuid", nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS || type != REG_SZ)
            return std::nullopt;
        // a GUID's characters are ASCII
        std::string guid;
        for (const wchar_t* c = value; *c; ++c)
        {
            if (*c > 0x7F)
                return std::nullopt;
            guid += static_cast<char>(*c);
        }
        if (guid.empty())
            return std::nullopt;
        return PcCodeOf(guid);
    }

    int Today()
    {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        return Day(time.wYear, time.wMonth, time.wDay);
    }

    void SetActive(const Key& key)
    {
        active = key;
    }

    const Key* Active()
    {
        return active ? &*active : nullptr;
    }
}  // namespace IWXMVM::Beta
