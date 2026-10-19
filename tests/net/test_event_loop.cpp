#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include "net/event_loop.hpp"

using namespace std::chrono_literals;
using rl::net::EventLoop;

TEST_CASE("event loop: stop() before run() makes run() return at once") {
    EventLoop loop;
    loop.stop();
    loop.run(); // would block forever (no fds, no timers) if the flag were ignored
    SUCCEED();
}

TEST_CASE("event loop: stop() from another thread wakes a blocked epoll_wait") {
    EventLoop loop;
    std::atomic<bool> returned{false};

    // No fds and no timers: run() sits in epoll_wait(-1). Only the eventfd
    // write in stop() can wake it; a bare flag would never be re-read.
    std::thread worker([&] {
        loop.run();
        returned.store(true);
    });
    std::this_thread::sleep_for(50ms); // let the worker block
    CHECK_FALSE(returned.load());

    loop.stop();
    worker.join(); // hangs here if the wakeup is missing
    CHECK(returned.load());
}

TEST_CASE("event loop: timers still fire after the wakeup fd was added") {
    EventLoop loop;
    int fired = 0;
    loop.add_timer(1ms, [&] {
        ++fired;
        loop.stop();
    });
    loop.run();
    CHECK(fired == 1);
}
