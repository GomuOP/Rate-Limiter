#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>
#include <vector>

#include "core/stats.hpp"

using rl::core::Counter;
using rl::core::Stats;

TEST_CASE("stats: Counter inc/get") {
    Counter c;
    CHECK(c.get() == 0);
    c.inc();
    c.inc(41);
    CHECK(c.get() == 42);
}

TEST_CASE("stats: relaxed increments from many threads are never lost") {
    constexpr int kThreads = 8;
    constexpr std::uint64_t kPerThread = 100'000;

    Counter c;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&c] {
            for (std::uint64_t i = 0; i < kPerThread; ++i) {
                c.inc();
            }
        });
    }
    for (auto& th : threads) {
        th.join(); // join is a synchronisation point: every inc() is visible after it
    }
    CHECK(c.get() == kThreads * kPerThread);
}

TEST_CASE("stats: to_json reports counters plus limiter-derived values") {
    Stats s;
    s.requests.inc(3);
    s.denied.inc();
    const auto j = s.to_json(/*key_count=*/7, /*evictions=*/2, /*uptime_ms=*/100);
    CHECK(j["requests"] == 3);
    CHECK(j["denied"] == 1);
    CHECK(j["allowed"] == 0);
    CHECK(j["key_count"] == 7);
    CHECK(j["evictions"] == 2);
    CHECK(j["uptime_ms"] == 100);
}
