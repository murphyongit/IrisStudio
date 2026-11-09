#include "StdInclude.hpp"
#include "Test.hpp"

#include <thread>

#include "Components/ListenerList.hpp"

using IWXMVM::Components::ListenerList;

// the mod's hooks go in on one thread while the UI's frames run on another: an addition under a frame's call moved the
// functions from under it ("bad function call", and the render thread stopped for good)
TEST("listeners: added while another thread calls them")
{
    constexpr std::uint32_t ROUNDS = 200, LISTENERS = 300;
    std::atomic<std::uint32_t> failures = 0;
    for (std::uint32_t round = 0; round < ROUNDS; ++round)
    {
        ListenerList listeners;
        std::atomic<std::uint32_t> calls = 0;
        std::atomic<bool> calling = false, done = false;
        std::thread caller([&] {
            while (!done)
            {
                try
                {
                    listeners.Call();
                }
                catch (const std::exception&)
                {
                    ++failures;
                }
                calling = true;
            }
        });
        // (the additions take less than the thread takes to start)
        while (!calling)
            std::this_thread::yield();
        for (std::uint32_t i = 0; i < LISTENERS; ++i)
            listeners.Add([&] { ++calls; });
        done = true;
        caller.join();

        // each once in a call
        calls = 0;
        listeners.Call();
        CHECK(calls == LISTENERS);
    }
    CHECK(failures == 0);
}

TEST("listeners: one can add another, which runs from the next call on")
{
    ListenerList listeners;
    std::uint32_t added = 0;
    std::uint32_t ran = 0;
    listeners.Add([&] {
        if (added++ == 0)
            listeners.Add([&] { ++ran; });
    });
    listeners.Call();
    CHECK(ran == 0);
    listeners.Call();
    CHECK(ran == 1);
}

TEST("listeners: none to call does nothing, and the order is the order added")
{
    ListenerList listeners;
    listeners.Call();
    std::vector<int> order;
    listeners.Add([&] { order.push_back(1); });
    listeners.Add([&] { order.push_back(2); });
    listeners.Call();
    CHECK((order == std::vector<int>{1, 2}));
}
