#include "StdInclude.hpp"
#include "Test.hpp"

#include "Beta/BetaKey.hpp"
#include "Beta/BetaKeys.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

using namespace IWXMVM;

// Beta keys (Beta/BetaKey): the PC code, dates, reading a key and checking it. The keys were
// made by tools/beta-keys/beta-keys.ps1 new -Test with the test pair (TEST_PUBLIC_KEY), so the script and this code
// are checked against each other; the PC codes are those of the plan's prototype for made-up MachineGuids.

namespace
{
    // the keys whole, as the script prints them
    // clang-format off
    // Alex, KZ18-TW01, until 31 Dec 2026, key 1
    constexpr std::string_view VALID =
        "IRIS-AQSfwo1wAWwBAQBBbGV4oaW-OljHTE7hRtvb0nnCGPOzODng4frQo1JnNBjAQrUs2mKI2Zs8KTuzmIbkpgjo9h4i3F2KL9TUBdidGe8rzA";
    // Alex, KZ18-TW01, until 1 Oct 2026, key 2
    constexpr std::string_view ENDED =
        "IRIS-AQSfwo1wAREBAgBBbGV4HMCO0azRrH5AiH6NoCqBMP0jfXF1ur6wQK53aTu55T17xcK9GfNsQnHq1jrb6wxXqyS8Vkx9ycTxiafU8gWGAw";
    // A, VES5-HAK9, until 1 Mar 2027, key 65535
    constexpr std::string_view SHORT_NAME =
        "IRIS-AQHbsliqaagB__9BWm7OI4BG7SHkC92vFY3n5RmFRHUijYbAz9b4O8k1znzxY71JMVcZpAlPq0E049eu_1JFF0qbY-68GmqkdBddAQ";
    // a name of 32 bytes, KZ18-TW01, until 31 Dec 2026, key 3
    constexpr std::string_view LONG_NAME =
        "IRIS-ASCfwo1wAWwBAwBab8OrIMOFbmdzdHLDtm0tS293YWxjenlrIEpyIElJSeAviHe5jieFtZf0ZTq_rfuIOCwu4Pa7hbmwqWFF0D6uBc_9O78"
        "LrP-OShFgiaB4bxRFZTKBb94wKR5S3xi-d5U";
    // VALID's contents signed by another key pair (the plan's prototype)
    constexpr std::string_view OTHER_PAIR =
        "IRIS-AQSfwo1wAWwBAQBBbGV4418yndQoAV2b3MiNNpi_SdSFC3eAjJ3lhYb1lX_0Y1zHGqtFAMbD5dRr_HGCwRQ5peQtNikPVxGOKJzwOjQgDw";
    // clang-format on

    constexpr std::string_view ALEX_PC = "KZ18TW01";
    constexpr std::string_view SAM_PC = "VES5HAK9";

    int Oct28()
    {
        return Beta::Day(2026, 10, 28);
    }

    Beta::Verdict CheckTest(std::string_view text, std::string_view pc = ALEX_PC, int today = Oct28(),
                            Beta::Key* found = nullptr)
    {
        return Beta::Check(text, pc, today, Beta::TEST_PUBLIC_KEY, found);
    }
}  // namespace

TEST("a PC's code comes from its MachineGuid, the same as the script's")
{
    CHECK(Beta::PcCodeOf("3f2a9c41-7d0e-4b8a-9e15-c6d2f0a8b713") == ALEX_PC);
    CHECK(Beta::PcCodeOf("3F2A9C41-7D0E-4B8A-9E15-C6D2F0A8B713") == ALEX_PC);
    CHECK(Beta::PcCodeOf("a81d44f0-2c6b-4e3e-b0a7-5d9c1e72f4aa") == SAM_PC);
    CHECK(Beta::FormatPcCode(ALEX_PC) == "KZ18-TW01");
}

TEST("a PC code is read back as typed, and a typo is caught by its last character")
{
    CHECK(Beta::ReadPcCode("KZ18-TW01") == std::string(ALEX_PC));
    CHECK(Beta::ReadPcCode("kz18-tw01") == std::string(ALEX_PC));
    CHECK(Beta::ReadPcCode(" KZ18 TWO1 ") == std::string(ALEX_PC));
    CHECK(Beta::ReadPcCode("KZL8TWO1") == std::string(ALEX_PC));
    CHECK(Beta::ReadPcCode("ves5hak9") == std::string(SAM_PC));
    CHECK(!Beta::ReadPcCode("KZ18-TW07"));
    CHECK(!Beta::ReadPcCode("KZ1B-TW01"));
    CHECK(!Beta::ReadPcCode("VES5-HAK8"));
    CHECK(!Beta::ReadPcCode("KZ18-TW0"));
    CHECK(!Beta::ReadPcCode("KZ18-TW011"));
    CHECK(!Beta::ReadPcCode("KZ18-TWU1"));
    CHECK(!Beta::ReadPcCode(""));
}

TEST("days count from 1 Jan 2026, leap years too")
{
    CHECK(Beta::Day(2026, 1, 1) == 0);
    CHECK(Beta::Day(2026, 12, 31) == 364);
    CHECK(Beta::Day(2027, 1, 1) == 365);
    CHECK(Beta::Day(2028, 3, 1) == 790);
    CHECK(Beta::DateText(0) == "1 Jan 2026");
    CHECK(Beta::DateText(364) == "31 Dec 2026");
    CHECK(Beta::DateText(790) == "1 Mar 2028");
}

TEST("a key holds the name, the PC code, the end date and its number")
{
    const auto key = Beta::FindKey(VALID);
    CHECK(key.has_value());
    if (!key)
        return;
    CHECK(key->name == "Alex");
    CHECK(key->pcCode == ALEX_PC);
    CHECK(key->endDay == Beta::Day(2026, 12, 31));
    CHECK(key->number == 1);
    CHECK(key->text == VALID);

    const auto shortName = Beta::FindKey(SHORT_NAME);
    CHECK(shortName && shortName->name == "A" && shortName->pcCode == SAM_PC && shortName->number == 65535 &&
          shortName->endDay == Beta::Day(2027, 3, 1));
    const auto longName = Beta::FindKey(LONG_NAME);
    CHECK(longName && longName->name == "Zo\xC3\xAB \xC3\x85ngstr\xC3\xB6m-Kowalczyk Jr III" &&
          longName->name.size() == 32);
}

TEST("a key works on its PC until its last day, and not after")
{
    Beta::Key key;
    CHECK(CheckTest(VALID, ALEX_PC, Oct28(), &key) == Beta::Verdict::Valid);
    CHECK(key.name == "Alex");
    CHECK(CheckTest(VALID, ALEX_PC, Beta::Day(2026, 12, 31)) == Beta::Verdict::Valid);
    CHECK(CheckTest(VALID, ALEX_PC, Beta::Day(2027, 1, 1)) == Beta::Verdict::Ended);
    CHECK(CheckTest(ENDED, ALEX_PC, Oct28(), &key) == Beta::Verdict::Ended);
    CHECK(key.endDay == Beta::Day(2026, 10, 1));
    CHECK(CheckTest(SHORT_NAME, SAM_PC) == Beta::Verdict::Valid);
    CHECK(CheckTest(LONG_NAME) == Beta::Verdict::Valid);
}

TEST("a key for another PC doesn't work, and says so before its end date does")
{
    CHECK(CheckTest(VALID, SAM_PC) == Beta::Verdict::OtherPc);
    CHECK(CheckTest(ENDED, SAM_PC) == Beta::Verdict::OtherPc);
    CHECK(CheckTest(SHORT_NAME, ALEX_PC) == Beta::Verdict::OtherPc);
    CHECK(CheckTest(VALID, "") == Beta::Verdict::NoPcCode);
}

TEST("a key with any character changed isn't valid")
{
    int notValid = 0;
    const std::string_view base64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (std::size_t i = 5; i < VALID.size(); ++i)
    {
        std::string changed(VALID);
        // the next character of the alphabet: every bit of the character can change, its last ones too
        changed[i] = base64Url[(base64Url.find(changed[i]) + 1) % base64Url.size()];
        if (CheckTest(changed) == Beta::Verdict::NotValid)
            ++notValid;
    }
    CHECK(notValid == static_cast<int>(VALID.size() - 5));
}

TEST("a key is found in a message, across line breaks, and not when cut short")
{
    std::string message = "Here's your key:\r\n";
    message += std::string(VALID.substr(0, 40)) + "\r\n" + std::string(VALID.substr(40, 30)) + " " +
               std::string(VALID.substr(70)) + "\r\nThanks for testing!";
    Beta::Key key;
    CHECK(CheckTest(message, ALEX_PC, Oct28(), &key) == Beta::Verdict::Valid);
    CHECK(key.text == VALID);

    const std::string cut(VALID.substr(0, VALID.size() - 10));
    CHECK(CheckTest(cut) == Beta::Verdict::NotValid);
    CHECK(CheckTest(cut + " thanks, see you") == Beta::Verdict::NotValid);
    CHECK(!Beta::FindKey(cut));
    // a newer format (its first byte isn't 1)
    CHECK(!Beta::FindKey("IRIS-B" + std::string(VALID.substr(6))));
}

TEST("only the key pair's keys are valid")
{
    CHECK(Beta::FindKey(OTHER_PAIR).has_value());
    CHECK(CheckTest(OTHER_PAIR) == Beta::Verdict::NotValid);
    // the test pair's keys in a build with the user's key
    CHECK(Beta::Check(VALID, ALEX_PC, Oct28(), Beta::PUBLIC_KEY) == Beta::Verdict::NotValid);
    CHECK(Beta::IsSet(Beta::TEST_PUBLIC_KEY));
}

TEST("no key is no key, not an invalid one")
{
    CHECK(CheckTest("") == Beta::Verdict::NoKey);
    CHECK(CheckTest("hello") == Beta::Verdict::NoKey);
    CHECK(!Beta::HasKeyPrefix("iris-AQSf"));
    CHECK(CheckTest("IRIS-") == Beta::Verdict::NotValid);
}

TEST("the launcher, About and the mod say what a key does")
{
    auto key = *Beta::FindKey(VALID);
    const auto ended = *Beta::FindKey(ENDED);
    CHECK(Beta::ValidLine(key) == "Beta key for Alex, until 31 Dec 2026");
    CHECK(Beta::KeyText(key) == "Alex, until 31 Dec 2026");
    CHECK(Beta::AboutLine(key) == "Beta: Alex, until 31 Dec 2026");
    CHECK(Beta::KeyScreenLine(Beta::Verdict::NoKey, {}) == "Send this code to get a beta key");
    CHECK(Beta::KeyScreenLine(Beta::Verdict::OtherPc, key) == "This key is for another PC");
    CHECK(Beta::KeyScreenLine(Beta::Verdict::Ended, ended) == "This beta ended on 1 Oct 2026");
    CHECK(Beta::KeyScreenLine(Beta::Verdict::NotValid, {}) == "This key isn't valid");
    CHECK(Beta::ModMessage(Beta::Verdict::Ended, "T5iris", ended, ALEX_PC) ==
          "This T5iris beta ended on 1 Oct 2026. Thanks for testing!");
    CHECK(Beta::ModMessage(Beta::Verdict::OtherPc, "T5iris", key, SAM_PC) ==
          "This beta key is for another PC. Send this PC's code, VES5-HAK9, to get a key for it, then paste it in "
          "Iris Studio.");
    CHECK(Beta::ModMessage(Beta::Verdict::NoKey, "T5iris", {}, SAM_PC).find("VES5-HAK9") != std::string::npos);
    CHECK(Beta::ModMessage(Beta::Verdict::NotValid, "T5iris", {}, SAM_PC) ==
          "This beta key isn't valid. Open Iris Studio and paste it again.");
    key.testKey = true;
    CHECK(Beta::AboutLine(key) == "Beta: Alex, until 31 Dec 2026 (test key)");
}

TEST("the mod finds the key next to its DLL first, then in the launcher's folder")
{
    const auto places = Beta::KeyFilePlaces(R"(C:\Games\mod)", R"(C:\Users\u\AppData\Local)");
    CHECK(places.size() == 2);
    CHECK(places.size() == 2 && places[0] == std::filesystem::path(R"(C:\Games\mod\beta.key)") &&
          places[1] == std::filesystem::path(R"(C:\Users\u\AppData\Local\IrisStudio\beta.key)"));
    CHECK(Beta::KeyFilePlaces(R"(C:\Users\u\AppData\Local\IrisStudio)", R"(C:\Users\u\AppData\Local)").size() == 1);

    const auto folder =
        std::filesystem::temp_directory_path() / std::format("iris-beta-test-{}", GetCurrentProcessId());
    std::error_code error;
    std::filesystem::create_directories(folder / "second", error);
    {
        std::ofstream(folder / "second" / "beta.key", std::ios::binary) << VALID;
    }
    CHECK(Beta::ReadKeyFile({folder / "first" / "beta.key", folder / "second" / "beta.key"}) == VALID);
    {
        std::ofstream(folder / "big.key", std::ios::binary) << std::string(5000, 'x');
    }
    CHECK(Beta::ReadKeyFile({folder / "big.key"}).empty());
    CHECK(Beta::ReadKeyFile({}).empty());
    std::filesystem::remove_all(folder, error);
}

TEST("this PC has a code, and today is a day")
{
    const auto code = Beta::ThisPcCode();
    CHECK(code.has_value() && Beta::ReadPcCode(*code) == code);
    CHECK(Beta::Today() >= Beta::Day(2026, 10, 28));
}

TEST("the mod's key is kept for About")
{
    CHECK(Beta::Active() == nullptr);
    Beta::SetActive(*Beta::FindKey(VALID));
    CHECK(Beta::Active() != nullptr && Beta::Active()->name == "Alex");
}
