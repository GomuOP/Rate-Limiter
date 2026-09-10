#include "core/config.hpp"

#include <fstream>
#include <stdexcept>

namespace rl::core {

namespace {

using nlohmann::json;

template <typename T> T get_or(const json& j, const char* key, T fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    return it->get<T>();
}

LimiterConfig::Algo parse_algo(const std::string& s) {
    if (s == "token_bucket") {
        return LimiterConfig::Algo::token_bucket;
    }
    if (s == "sliding_window") {
        return LimiterConfig::Algo::sliding_window;
    }
    throw std::runtime_error("limiter.algo must be 'token_bucket' or 'sliding_window', got '" + s +
                             "'");
}

KeyExtractorConfig::Source parse_source(const std::string& s) {
    if (s == "query") {
        return KeyExtractorConfig::Source::query;
    }
    if (s == "header") {
        return KeyExtractorConfig::Source::header;
    }
    if (s == "ip") {
        return KeyExtractorConfig::Source::ip;
    }
    throw std::runtime_error("key_extractor.source must be 'query', 'header' or 'ip', got '" + s +
                             "'");
}

Config::Mode parse_mode(const std::string& s) {
    if (s == "local") {
        return Config::Mode::local;
    }
    if (s == "redis") {
        return Config::Mode::redis;
    }
    throw std::runtime_error("mode must be 'local' or 'redis', got '" + s + "'");
}

} // namespace

Config Config::from_json(const json& j) {
    Config c;

    c.mode = parse_mode(get_or<std::string>(j, "mode", "local"));

    if (auto it = j.find("server"); it != j.end()) {
        const auto& s = *it;
        c.server.host = get_or<std::string>(s, "host", c.server.host);
        c.server.port = get_or<std::uint16_t>(s, "port", c.server.port);
        c.server.backlog = get_or<int>(s, "backlog", c.server.backlog);
        c.server.max_header_bytes =
            get_or<std::size_t>(s, "max_header_bytes", c.server.max_header_bytes);
        c.server.max_body_bytes = get_or<std::size_t>(s, "max_body_bytes", c.server.max_body_bytes);
        c.server.idle_timeout = std::chrono::milliseconds(
            get_or<std::int64_t>(s, "idle_timeout_ms", c.server.idle_timeout.count()));
    }

    if (auto it = j.find("limiter"); it != j.end()) {
        const auto& l = *it;
        c.limiter.algo = parse_algo(get_or<std::string>(l, "algo", "token_bucket"));
        c.limiter.rate = get_or<double>(l, "rate", c.limiter.rate);
        c.limiter.burst = get_or<std::int64_t>(l, "burst", c.limiter.burst);
        c.limiter.limit = get_or<std::int64_t>(l, "limit", c.limiter.limit);
        c.limiter.window = std::chrono::milliseconds(
            get_or<std::int64_t>(l, "window_ms", c.limiter.window.count()));
    }

    if (auto it = j.find("key_store"); it != j.end()) {
        const auto& k = *it;
        c.key_store.max_keys = get_or<std::size_t>(k, "max_keys", c.key_store.max_keys);
        c.key_store.idle_ttl = std::chrono::milliseconds(
            get_or<std::int64_t>(k, "idle_ttl_ms", c.key_store.idle_ttl.count()));
        c.key_store.sweep_interval = std::chrono::milliseconds(
            get_or<std::int64_t>(k, "sweep_interval_ms", c.key_store.sweep_interval.count()));
    }

    if (auto it = j.find("key_extractor"); it != j.end()) {
        const auto& k = *it;
        c.key_extractor.source = parse_source(get_or<std::string>(k, "source", "query"));
        c.key_extractor.param = get_or<std::string>(k, "param", c.key_extractor.param);
        c.key_extractor.trust_forwarded_for =
            get_or<bool>(k, "trust_forwarded_for", c.key_extractor.trust_forwarded_for);
        c.key_extractor.fallback_ip = get_or<bool>(k, "fallback_ip", c.key_extractor.fallback_ip);
    }

    if (auto it = j.find("redis"); it != j.end()) {
        const auto& r = *it;
        c.redis.host = get_or<std::string>(r, "host", c.redis.host);
        c.redis.port = get_or<std::uint16_t>(r, "port", c.redis.port);
        c.redis.timeout_ms = get_or<int>(r, "timeout_ms", c.redis.timeout_ms);
        c.redis.fail_closed = get_or<bool>(r, "fail_closed", c.redis.fail_closed);
        c.redis.connect_backoff_ms =
            get_or<int>(r, "connect_backoff_ms", c.redis.connect_backoff_ms);
    }

    c.validate();
    return c;
}

Config Config::from_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open config file: " + path);
    }
    json j;
    try {
        in >> j;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("config parse error in " + path + ": " + e.what());
    }
    return from_json(j);
}

void Config::validate() const {
    if (server.port == 0) {
        throw std::runtime_error("server.port must be non-zero");
    }
    if (server.max_header_bytes < 256) {
        throw std::runtime_error("server.max_header_bytes must be >= 256");
    }
    if (limiter.algo == LimiterConfig::Algo::token_bucket) {
        if (!(limiter.rate > 0.0)) {
            throw std::runtime_error("limiter.rate must be > 0");
        }
        if (limiter.burst < 1) {
            throw std::runtime_error("limiter.burst must be >= 1");
        }
    } else {
        if (limiter.limit < 1) {
            throw std::runtime_error("limiter.limit must be >= 1");
        }
        if (limiter.window.count() <= 0) {
            throw std::runtime_error("limiter.window_ms must be > 0");
        }
    }
    if (key_store.max_keys < 1) {
        throw std::runtime_error("key_store.max_keys must be >= 1");
    }
    if (key_store.idle_ttl.count() <= 0) {
        throw std::runtime_error("key_store.idle_ttl_ms must be > 0");
    }
    if (redis.timeout_ms < 1) {
        throw std::runtime_error("redis.timeout_ms must be >= 1");
    }
}

} // namespace rl::core
