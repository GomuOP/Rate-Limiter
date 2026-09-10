#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "core/clock.hpp"
#include "limit/token_bucket.hpp"

using namespace std::chrono_literals;
using rl::core::ManualClock;
using rl::limit::TokenBucketLimiter;

namespace {
constexpr std::size_t kMaxKeys = 1024;
constexpr auto kIdle = 10min;
} // namespace

TEST_CASE("token bucket: fresh key starts full") {
    ManualClock clk;
    TokenBucketLimiter tb(10.0, 10, clk, kMaxKeys, kIdle);

    auto d = tb.check("a");
    CHECK(d.allowed);
    CHECK(d.limit == 10);
    CHECK(d.remaining == 9);
    CHECK(d.retry_after == 0ms);
}

TEST_CASE("token bucket: exhausts at burst then denies with exact retry_after") {
    ManualClock clk;
    TokenBucketLimiter tb(10.0, 10, clk, kMaxKeys, kIdle); // 10 tokens, +10/sec

    for (int i = 0; i < 10; ++i) {
        INFO("iteration " << i);
        CHECK(tb.check("k").allowed);
    }
    auto denied = tb.check("k");
    CHECK_FALSE(denied.allowed);
    CHECK(denied.remaining == 0);
    CHECK(denied.retry_after == 100ms); // one token at 10/sec
    CHECK(denied.reset == 1000ms);      // ten tokens at 10/sec
}

TEST_CASE("token bucket: lazy refill is proportional to elapsed time") {
    ManualClock clk;
    TokenBucketLimiter tb(10.0, 10, clk, kMaxKeys, kIdle);

    for (int i = 0; i < 10; ++i) {
        REQUIRE(tb.check("k").allowed);
    }
    REQUIRE_FALSE(tb.check("k").allowed);

    clk.advance(250ms); // +2.5 tokens
    CHECK(tb.check("k").allowed);
    CHECK(tb.check("k").allowed);
    CHECK_FALSE(tb.check("k").allowed); // 0.5 left, not enough
}

TEST_CASE("token bucket: refill is capped at burst") {
    ManualClock clk;
    TokenBucketLimiter tb(10.0, 10, clk, kMaxKeys, kIdle);

    REQUIRE(tb.check("k").allowed); // 9 left
    clk.advance(1h);                // would be +36000 tokens, capped to 10

    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
        if (tb.check("k").allowed) {
            ++allowed;
        }
    }
    CHECK(allowed == 10);
}

TEST_CASE("token bucket: cost greater than one") {
    ManualClock clk;
    TokenBucketLimiter tb(10.0, 10, clk, kMaxKeys, kIdle);

    auto d = tb.check("k", 4);
    CHECK(d.allowed);
    CHECK(d.remaining == 6);

    auto d2 = tb.check("k", 7); // only 6 left
    CHECK_FALSE(d2.allowed);
    CHECK(d2.retry_after == 100ms); // need 1 more token at 10/sec
}

TEST_CASE("token bucket: keys are independent") {
    ManualClock clk;
    TokenBucketLimiter tb(1.0, 2, clk, kMaxKeys, kIdle);

    CHECK(tb.check("a").allowed);
    CHECK(tb.check("a").allowed);
    CHECK_FALSE(tb.check("a").allowed);

    CHECK(tb.check("b").allowed); // b has its own full bucket
    CHECK(tb.check("b").allowed);
    CHECK_FALSE(tb.check("b").allowed);

    CHECK(tb.key_count() == 2);
}
