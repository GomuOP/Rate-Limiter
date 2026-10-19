#pragma once

#include <cstdint>
#include <string>

#include "core/config.hpp"
#include "core/stats.hpp"
#include "limit/rate_limiter.hpp"
#include "redis/redis_client.hpp"

namespace rl::redis {

/// Distributed token-bucket limiter backed by one Redis node (design 0.6, 0.7).
/// The bucket update is a single Lua script run with EVALSHA (EVAL fallback on
/// NOSCRIPT). When Redis is unreachable the limiter fails open by default, or
/// closed if `redis.fail_closed` is set; either way a stats counter is bumped.
class RedisRateLimiter final : public limit::RateLimiter {
  public:
    RedisRateLimiter(const core::RedisConfig& rcfg,
                     double rate,
                     std::int64_t burst,
                     core::Stats& stats);

    limit::Decision check(std::string_view key, std::int64_t cost = 1) override;
    std::size_t key_count() const override { return 0; } // Redis owns the keys
    std::size_t sweep_expired() override { return 0; }   // PEXPIRE handles it
    std::size_t evictions() const override { return 0; } // no local key store

  private:
    bool load_script();
    limit::Decision fail_decision(std::int64_t cost);

    RedisClient client_;
    std::string sha_; // empty until the script is loaded
    double rate_;
    std::int64_t burst_;
    bool fail_closed_;
    core::Stats& stats_;
};

} // namespace rl::redis
