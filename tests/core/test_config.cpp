#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "core/config.hpp"

using nlohmann::json;
using rl::core::Config;
using rl::core::KeyExtractorConfig;
using rl::core::LimiterConfig;

TEST_CASE("config: empty object yields defaults") {
    Config c = Config::from_json(json::object());
    CHECK(c.mode == Config::Mode::local);
    CHECK(c.server.port == 8080);
    CHECK(c.limiter.algo == LimiterConfig::Algo::token_bucket);
    CHECK(c.limiter.rate == 100.0);
    CHECK(c.key_store.max_keys == 100'000);
}

TEST_CASE("config: full document is parsed") {
    json j = {
        {"mode", "redis"},
        {"server", {{"host", "127.0.0.1"}, {"port", 9000}, {"max_header_bytes", 4096}}},
        {"limiter", {{"algo", "sliding_window"}, {"limit", 50}, {"window_ms", 2000}}},
        {"key_store", {{"max_keys", 10}, {"idle_ttl_ms", 1000}}},
        {"key_extractor", {{"source", "header"}, {"param", "X-Api-Key"}, {"fallback_ip", false}}},
        {"redis", {{"host", "redis.internal"}, {"port", 6380}, {"fail_closed", true}}},
    };
    Config c = Config::from_json(j);

    CHECK(c.mode == Config::Mode::redis);
    CHECK(c.server.host == "127.0.0.1");
    CHECK(c.server.port == 9000);
    CHECK(c.limiter.algo == LimiterConfig::Algo::sliding_window);
    CHECK(c.limiter.limit == 50);
    CHECK(c.limiter.window.count() == 2000);
    CHECK(c.key_store.max_keys == 10);
    CHECK(c.key_extractor.source == KeyExtractorConfig::Source::header);
    CHECK(c.key_extractor.param == "X-Api-Key");
    CHECK_FALSE(c.key_extractor.fallback_ip);
    CHECK(c.redis.host == "redis.internal");
    CHECK(c.redis.port == 6380);
    CHECK(c.redis.fail_closed);
}

TEST_CASE("config: invalid enum strings throw") {
    CHECK_THROWS(Config::from_json(json{{"mode", "cluster"}}));
    CHECK_THROWS(Config::from_json(json{{"limiter", {{"algo", "leaky"}}}}));
    CHECK_THROWS(Config::from_json(json{{"key_extractor", {{"source", "cookie"}}}}));
}

TEST_CASE("config: validation rejects nonsensical values") {
    CHECK_THROWS(Config::from_json(json{{"limiter", {{"algo", "token_bucket"}, {"rate", 0}}}}));
    CHECK_THROWS(Config::from_json(json{{"limiter", {{"algo", "token_bucket"}, {"burst", 0}}}}));
    CHECK_THROWS(Config::from_json(json{{"limiter", {{"algo", "sliding_window"}, {"limit", 0}}}}));
    CHECK_THROWS(Config::from_json(json{{"server", {{"port", 0}}}}));
}
