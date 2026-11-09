#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Beta keys: a key, signed on the user's PC by tools/beta-keys/beta-keys.ps1, holds a
// tester's name, their PC's code and an end date. A beta build checks it, in the launcher and in the mod; the default
// build has no check. Only the public key is in the app, so it can check keys but not make them.
namespace IWXMVM::Beta
{
    // a P-256 public key: X, then Y, 32 bytes each, big-endian
    using PublicKey = std::array<std::uint8_t, 64>;

    // the file a key is kept in, in the launcher's folder
    inline constexpr std::string_view KEY_FILE = "beta.key";

    // A PC's code: 7 characters of Crockford's base32 from a hash of Windows' install ID (MachineGuid), then one that
    // checks them, as 8 characters without the dash ("KZ18TW01")
    std::string PcCodeOf(std::string_view machineGuid);
    // "KZ18-TW01"
    std::string FormatPcCode(std::string_view code);
    // a code as typed or pasted: any case, a dash or spaces, O for 0, I and L for 1; none when it isn't 8 characters
    // of the alphabet or its last one doesn't check the others
    std::optional<std::string> ReadPcCode(std::string_view text);

    // dates as days since 2026-01-01, the way keys hold them
    int Day(int year, int month, int day);
    // "31 Dec 2026"
    std::string DateText(int day);

    struct Key
    {
        std::string name;      // the tester's, UTF-8
        std::string pcCode;    // 8 characters
        int endDay = 0;        // the last day it works, on the PC's own date
        int number = 0;        // in the user's list of keys issued
        std::string text;      // the key as written ("IRIS-..."), without the spaces or line breaks found in it
        bool testKey = false;  // checked with the test key pair (/p:IrisBeta=test builds), for the About line

        std::vector<std::uint8_t> signedBytes;     // what the signature is of
        std::array<std::uint8_t, 64> signature{};  // r, then s
    };

    // The key in a text (what was pasted, a beta.key): found by its IRIS- prefix, its length read from its header, with
    // spaces and line breaks inside it skipped. None when the text has none, or one that is cut short, damaged or of a
    // newer format.
    std::optional<Key> FindKey(std::string_view text);
    // whether a text has a key's prefix at all ("The clipboard has no key")
    bool HasKeyPrefix(std::string_view text);

    // whether a signature of `message` is good for `publicKey` (Windows' CNG)
    bool VerifySignature(const PublicKey& publicKey, std::span<const std::uint8_t> message,
                         std::span<const std::uint8_t, 64> signature);

    enum class Verdict
    {
        Valid,
        NoKey,     // no key in the text
        NotValid,  // damaged, cut short, of a newer format, or not signed by the key pair
        OtherPc,
        Ended,
        NoPcCode,  // this PC's code couldn't be read
    };

    // The check of a key in `text` for this PC (its code, empty when it couldn't be read) on `today`. The key found
    // goes to `found` when it is one, so its name and end date can be shown.
    Verdict Check(std::string_view text, std::string_view thisPc, int today, const PublicKey& publicKey,
                  Key* found = nullptr);

    // the launcher's line under the PC code for a key that doesn't work, or before one was pasted
    std::string KeyScreenLine(Verdict verdict, const Key& key);
    inline constexpr std::string_view NO_KEY_ON_CLIPBOARD = "The clipboard has no key";
    // "Beta key for Alex, until 31 Dec 2026": the home view's line once a key worked
    std::string ValidLine(const Key& key);
    // "Alex, until 31 Dec 2026": Settings' row
    std::string KeyText(const Key& key);
    // "Beta: Alex, until 31 Dec 2026": the mod's About window
    std::string AboutLine(const Key& key);
    // the mod's message box when it refuses to start; `thisPc` is this PC's code, for the cases a new key answers
    std::string ModMessage(Verdict verdict, std::string_view modName, const Key& key, std::string_view thisPc);

    // where the mod looks for the key: next to its DLL (the launcher's folder, which tests can move), then the
    // launcher's own folder in %LOCALAPPDATA%
    std::vector<std::filesystem::path> KeyFilePlaces(const std::filesystem::path& dllFolder,
                                                     const std::filesystem::path& localAppData);
    // the text of the first of them there is; empty when none is
    std::string ReadKeyFile(const std::vector<std::filesystem::path>& places);

    // this PC's code, from MachineGuid (read from the 64-bit registry, also by 32-bit programs); none if it can't be
    // read
    std::optional<std::string> ThisPcCode();
    // today, on the PC's own date
    int Today();

    // the key the mod started with, for the About window; none in a build without the check
    void SetActive(const Key& key);
    const Key* Active();
}  // namespace IWXMVM::Beta
