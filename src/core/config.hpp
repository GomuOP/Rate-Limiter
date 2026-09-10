#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace rl::core {

struct ServerConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = 8080;
    int backlog = 1024;
    std::size_t max_header_bytes = 8192;
    std::size_t max_body_bytes = 65536; // bodies are drained, not read
    std::chrono::milliseconds idle_timeout{15000};
};

struct LimiterConfig {
    enum class Algo { token_bucket, sliding_window };

    Algo algo = Algo::token_bucket;
    // token bucket
    double rate = 100.0;      // tokens added per second
    std::int64_t burst = 200; // bucket capacity
    // sliding window
    std::int64_t limit = 100; // max requests per window
    std::chrono::milliseconds window{1000};

    /// Throws std::runtime_error when the fields for the selected algo are
    /// out of range. Shared by startup config validation and the live
    /// /config endpoint.
    void validate() const;
};

struct KeyStoreConfig {
    std::size_t max_keys = 100'000;
    std::chrono::milliseconds idle_ttl{600'000};
    std::chrono::milliseconds sweep_interval{5'000};
};

struct KeyExtractorConfig {
    enum class Source { query, header, ip };

    Source source = Source::query;
    std::string param = "key"; // query param name or header name
    bool trust_forwarded_for = false;
    bool fallback_ip = true; // on missing key: true -> use peer IP, false -> 400
};

struct RedisConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    int timeout_ms = 50;
    bool fail_closed = false; // design 0.6: default fail-open
    int connect_backoff_ms = 500;
};

struct Config {
    enum class Mode { local, redis };

    Mode mode = Mode::local;
    ServerConfig server;
    LimiterConfig limiter;
    KeyStoreConfig key_store;
    KeyExtractorConfig key_extractor;
    RedisConfig redis;

    static Config from_json(const nlohmann::json& j);
    static Config from_json_file(const std::string& path);

    /// Throws std::runtime_error on an invalid combination of values.
    void validate() const;
};

} // namespace rl::core
