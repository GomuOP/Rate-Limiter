#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "limit/sharded_limiter.hpp"
#include "limit/token_bucket.hpp"

using namespace std::chrono_literals;
using rl::core::ManualClock;
using rl::limit::ShardedLimiter;
using rl::limit::TokenBucketLimiter;

namespace {

constexpr std::size_t kMaxKeys = 100'000;
constexpr auto kIdle = 10min;

/// A ShardedLimiter whose shards are token buckets with the given burst.
/// The rate is tiny and the ManualClock never moves during a test, so no
/// tokens refill: a key allows exactly `burst` requests, ever.
std::unique_ptr<ShardedLimiter>
make_sharded(const ManualClock& clk, std::size_t shards, std::int64_t burst) {
    return std::make_unique<ShardedLimiter>(shards, [&clk, burst] {
        return std::make_unique<TokenBucketLimiter>(/*rate=*/0.001, burst, clk, kMaxKeys, kIdle);
    });
}

} // namespace

TEST_CASE("sharded: single thread allows exactly burst per key") {
    ManualClock clk;
    auto lim = make_sharded(clk, /*shards=*/8, /*burst=*/5);

    for (int i = 0; i < 5; ++i) {
        CHECK(lim->check("a").allowed);
    }
    CHECK_FALSE(lim->check("a").allowed); // 6th request for "a" is denied
    CHECK(lim->check("b").allowed);       // "b" has its own bucket
    CHECK(lim->key_count() == 2);
}

TEST_CASE("sharded: zero shards is rejected") {
    ManualClock clk;
    CHECK_THROWS_AS(make_sharded(clk, /*shards=*/0, /*burst=*/5), std::invalid_argument);
}

TEST_CASE("sharded: 8 threads on ONE key allow exactly burst", "[sharded]") {
    constexpr int kThreads = 8, kPerThread = 1000, kBurst = 500;
    ManualClock clk;
    auto lim = make_sharded(clk, /*shards=*/16, kBurst);
    std::vector<int> per_thread(kThreads, 0);
    std::latch go(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] { // [&, t]: share lim/go/per_thread, copy t
            go.arrive_and_wait();     // start all threads at once: max contention
            for (int i = 0; i < kPerThread; ++i) {
                if (lim->check("hot").allowed) {
                    per_thread[t] += 1; // own slot only: no shared counter, no race
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join(); // join = all per_thread writes now visible
    }

    int total = 0;
    for (int c : per_thread) {
        total += c;
    }
    CHECK(total == kBurst); // exactly, not "about": a lost update shows here
    CHECK(lim->key_count() == 1);
}

TEST_CASE("sharded: 8 threads on MANY keys allow exactly burst each", "[sharded]") {
    constexpr int kThreads = 8, kKeys = 200, kRounds = 10, kBurst = 50;
    ManualClock clk;
    auto lim = make_sharded(clk, /*shards=*/16, kBurst);

    std::vector<int> per_thread(kThreads, 0);
    std::latch go(kThreads);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            go.arrive_and_wait();
            for (int r = 0; r < kRounds; ++r) {
                for (int i = 0; i < kKeys; ++i) { // ...over every key
                    if (lim->check("key-" + std::to_string(i)).allowed) {
                        per_thread[t] += 1;
                    }
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    int total = 0;
    for (int c : per_thread) {
        total += c;
    }
    CHECK(total == kKeys * kBurst); // every bucket drained exactly
    CHECK(lim->key_count() == kKeys);
}
