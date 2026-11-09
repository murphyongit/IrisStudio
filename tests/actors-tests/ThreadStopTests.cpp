#include "StdInclude.hpp"
#include "Test.hpp"

#include "Components/ThreadStopLogic.hpp"

#include "../../t5/src/PoolGrowthTables.hpp"

using namespace IWXMVM::Components::ThreadStopLogic;
namespace Tables = IWXMVM::T5::PoolGrowth::Tables;

namespace
{
    std::array<std::uint8_t, BYTES_BEFORE> Before(std::initializer_list<std::uint8_t> code)
    {
        std::array<std::uint8_t, BYTES_BEFORE> bytes{};
        std::ranges::copy(code, bytes.end() - code.size());
        return bytes;
    }

    bool InTable(std::uint32_t address)
    {
        const auto in = [&](const auto& function) { return address >= function.first && address < function.second; };
        return std::ranges::any_of(Tables::StreamingFunctions, in) || std::ranges::any_of(Tables::ZoneFunctions, in);
    }

    bool Always(std::uint32_t)
    {
        return true;
    }

    bool Never(std::uint32_t)
    {
        return false;
    }

    bool FromOnly(std::uint32_t address)
    {
        return address >= 0x1000 && address < 0x2000;
    }
}  // namespace

TEST("thread stop: a return address follows a call, of any form")
{
    CHECK(FollowsCall(Before({0xE8, 0x10, 0x20, 0x30, 0x40})));              // call rel32
    CHECK(FollowsCall(Before({0xFF, 0xD0})));                                // call eax
    CHECK(FollowsCall(Before({0xFF, 0x10})));                                // call [eax]
    CHECK(FollowsCall(Before({0xFF, 0x50, 0x08})));                          // call [eax+8]
    CHECK(FollowsCall(Before({0xFF, 0x15, 0x10, 0x20, 0x30, 0x40})));        // call [address]
    CHECK(FollowsCall(Before({0xFF, 0x14, 0x85, 0x10, 0x20, 0x30, 0x40})));  // call [eax*4+address]
}

TEST("thread stop: the address of the call inputhost.dll's thread kept isn't a return address")
{
    // 0x718870 is the call itself, after three pushes
    CHECK(!FollowsCall(Before({0x89, 0x96, 0x2C, 0x51, 0x57, 0x52, 0x50})));
    // other jumps and moves
    CHECK(!FollowsCall(Before({0xFF, 0xE0})));  // jmp eax
    CHECK(!FollowsCall(Before({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})));
}

TEST("thread stop: the instruction pointer counts with or without a call before it")
{
    const std::array<std::uint32_t, 2> stack{1, 2};
    const auto match = FindInCode(0x1500, stack, FromOnly, Never);
    CHECK(match.has_value());
    CHECK(match->address == 0x1500);
    CHECK(match->depth == -1);
}

TEST("thread stop: a word in the code that follows no call doesn't count, as long as it's there")
{
    const std::array<std::uint32_t, 3> stack{5, 0x1500, 7};
    CHECK(!FindInCode(0x500, stack, FromOnly, Never).has_value());
    const auto match = FindInCode(0x500, stack, FromOnly, Always);
    CHECK(match.has_value());
    CHECK(match->address == 0x1500);
    CHECK(match->depth == 1);
}

TEST("thread stop: a frame far up the stack counts, past where the first version looked")
{
    std::vector<std::uint32_t> stack(0x4000, 7);
    stack[0x3FFF] = 0x1234;
    const auto match = FindInCode(0x500, stack, FromOnly, Always);
    CHECK(match.has_value());
    CHECK(match->depth == 0x3FFF);
}

TEST("thread stop: no stack, nothing in the way")
{
    CHECK(!FindInCode(0x500, {}, FromOnly, Always).has_value());
}

TEST("thread stop tables: every site that changes lies in a function that's waited for")
{
    for (const auto& write : Tables::ImageWrites)
        CHECK(InTable(write.site));
    for (const auto& write : Tables::ModelWrites)
        CHECK(InTable(write.site));
    // (but the one byte that turns the count's jump into a jmp: no thread can be caught halfway through that)
    for (const auto& write : Tables::ZoneWrites)
        CHECK(write.kind == Tables::Kind::Byte || InTable(write.site));
}

TEST("thread stop tables: the render thread's main loop and what kept growth waiting aren't in them")
{
    // measured on 2026-11-07: the render thread's frames, and the word a thread of inputhost.dll held
    for (const std::uint32_t address : {0x718788u, 0x718A74u, 0x718773u, 0x718BB2u, 0x717E1Fu, 0x718674u, 0x718870u})
        CHECK(!InTable(address));
    // a zone's load, which is really in the way
    CHECK(InTable(0x80E0CC));
}
