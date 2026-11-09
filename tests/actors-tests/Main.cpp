#include "Test.hpp"

#include <cstring>
#include <exception>

// actors-tests [text]: runs every test, or those whose name has the text
int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failedCases = 0;
    std::size_t run = 0;
    for (const auto& testCase : Test::Cases())
    {
        if (filter && !std::strstr(testCase.name, filter))
            continue;
        ++run;
        const auto before = Test::Failures();
        // an exception fails its test, not the run
        try
        {
            testCase.body();
        }
        catch (const std::exception& e)
        {
            Test::Fail(testCase.name, 0, std::string("threw: ") + e.what());
        }
        const bool passed = Test::Failures() == before;
        failedCases += passed ? 0 : 1;
        std::printf("%s %s\n", passed ? "PASS" : "FAIL", testCase.name);
        std::fflush(stdout);
    }
    std::printf("%zu tests, %d failed\n", run, failedCases);
    return failedCases == 0 ? 0 : 1;
}
