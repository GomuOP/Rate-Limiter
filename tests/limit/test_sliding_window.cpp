#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "core/clock.hpp"
#include "limit/sliding_window.hpp"

using namespace std::chrono_literals;
using rl::core::ManualClock;
using rl::limit::SlidingWindowLimiter;

namespace {
constexpr std::size_t kMaxKeys = 1024;
constexpr auto kIdle = 10min;
} // namespace

TEST_CASE("sliding window: allows exactly `limit` in an empty window") {
    ManualClock clk; // starts at epoch -> window index 0, position 0
    SlidingWindowLimiter sw(10, 1000ms, clk, kMaxKeys, kIdle);

    for (int i = 0; i < 10; ++i) {
        INFO("iteration " << i);
        CHECK(sw.check("k").allowed);
    }
    auto denied = sw.check("k");
    CHECK_FALSE(denied.allowed);
    CHECK(denied.retry_after == 1000ms); // no previous window: wait for the boundary
}

TEST_CASE("sliding window: previous window is fully weighted at the boundary") {
    ManualClock clk;
    SlidingWindowLimiter sw(10, 1000ms, clk, kMaxKeys, kIdle);

    for (int i = 0; i < 10; ++i) {
        REQUIRE(sw.check("k").allowed);
    }
    clk.advance(1000ms); // exactly on the next window boundary

    auto d = sw.check("k");
    CHECK_FALSE(d.allowed); // prev counts 10 at weight 1.0
    CHECK(d.retry_after == 100ms);
}

TEST_CASE("sliding window: previous window decays linearly through the window") {
    ManualClock clk;
    SlidingWindowLimiter sw(10, 1000ms, clk, kMaxKeys, kIdle);

    for (int i = 0; i < 10; ++i) {
        REQUIRE(sw.check("k").allowed);
    }
    clk.advance(1100ms); // 100ms into the next window: prev weight 0.9 -> estimate 9

    CHECK(sw.check("k").allowed);       // 9 + 1 <= 10
    CHECK_FALSE(sw.check("k").allowed); // now 10
}

TEST_CASE("sliding window: no 2x burst across a boundary") {
    ManualClock clk;
    SlidingWindowLimiter sw(10, 1000ms, clk, kMaxKeys, kIdle);

    clk.advance(900ms); // near the end of window 0
    int burst = 0;
    for (int i = 0; i < 20; ++i) {
        if (sw.check("k").allowed) {
            ++burst;
        }
    }
    CHECK(burst == 10);

    clk.advance(200ms); // 100ms into window 1 -> spans the boundary
    int after = 0;
    for (int i = 0; i < 20; ++i) {
        if (sw.check("k").allowed) {
            ++after;
        }
    }
    CHECK(after == 1); // 10 + 1 across the boundary, nowhere near 20
}

TEST_CASE("sliding window: fully clears after two idle windows") {
    ManualClock clk;
    SlidingWindowLimiter sw(5, 1000ms, clk, kMaxKeys, kIdle);

    for (int i = 0; i < 5; ++i) {
        REQUIRE(sw.check("k").allowed);
    }
    REQUIRE_FALSE(sw.check("k").allowed);

    clk.advance(2000ms); // two windows later: prev and cur both reset
    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (sw.check("k").allowed) {
            ++allowed;
        }
    }
    CHECK(allowed == 5);
}

TEST_CASE("sliding window: keys are independent") {
    ManualClock clk;
    SlidingWindowLimiter sw(2, 1000ms, clk, kMaxKeys, kIdle);

    CHECK(sw.check("a").allowed);
    CHECK(sw.check("a").allowed);
    CHECK_FALSE(sw.check("a").allowed);
    CHECK(sw.check("b").allowed);
    CHECK(sw.check("b").allowed);
    CHECK_FALSE(sw.check("b").allowed);
    CHECK(sw.key_count() == 2);
}
