#pragma once
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// A minimal test runner: TEST("name") { CHECK(...); } registers a test, and Main.cpp runs them all. A failed check
// prints where it is and the test goes on; the run fails if any check did.

namespace Test
{
    struct Case
    {
        const char* name;
        std::function<void()> body;
    };

    inline std::vector<Case>& Cases()
    {
        static std::vector<Case> cases;
        return cases;
    }

    inline int& Failures()
    {
        static int failures = 0;
        return failures;
    }

    struct Registrar
    {
        Registrar(const char* name, std::function<void()> body)
        {
            Cases().push_back({name, std::move(body)});
        }
    };

    inline void Fail(const char* file, int line, const std::string& what)
    {
        ++Failures();
        std::printf("  FAIL %s:%d: %s\n", file, line, what.c_str());
    }
}  // namespace Test

#define TEST_CONCAT2(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT2(a, b)
#define TEST(name)                                                                              \
    static void TEST_CONCAT(test_, __LINE__)();                                                 \
    static Test::Registrar TEST_CONCAT(registrar_, __LINE__)(name, TEST_CONCAT(test_, __LINE__)); \
    static void TEST_CONCAT(test_, __LINE__)()

#define CHECK(condition)                                    \
    do                                                      \
    {                                                       \
        if (!(condition))                                   \
            Test::Fail(__FILE__, __LINE__, #condition);     \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                                              \
    do                                                                                                       \
    {                                                                                                        \
        const double testActual = static_cast<double>(actual);                                               \
        const double testExpected = static_cast<double>(expected);                                           \
        if (!(std::abs(testActual - testExpected) <= static_cast<double>(tolerance)))                         \
            Test::Fail(__FILE__, __LINE__,                                                                   \
                       std::string(#actual) + " is " + std::to_string(testActual) + ", expected " +          \
                           std::to_string(testExpected) + " +- " + std::to_string(static_cast<double>(tolerance))); \
    } while (false)
