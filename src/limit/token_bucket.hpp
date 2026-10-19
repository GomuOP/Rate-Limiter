#pragma once

#include "core/clock.hpp"
#include "limit/key_store.hpp"
#include "limit/rate_limiter.hpp"

namespace rl::limit {

/// Token bucket with lazy refill (design 0.2): each key holds a token count and
/// a timestamp; elapsed time is converted to tokens on every `check()`. No timer
/// touches bucket state, so idle keys cost nothing until swept.
class TokenBucketLimiter final : public RateLimiter {
  public:
    TokenBucketLimiter(double rate,
                       std::int64_t burst,
                       const core::Clock& clock,
                       std::size_t max_keys,
                       core::Duration idle_ttl);

    Decision check(std::string_view key, std::int64_t cost = 1) override;
    std::size_t key_count() const override { return store_.size(); }
    std::size_t sweep_expired() override { return store_.sweep_expired(); }
    std::size_t evictions() const override { return store_.evictions(); }

  private:
    struct Bucket {
        double tokens;
        core::TimePoint last;
    };

    double rate_;  // tokens per second
    double burst_; // capacity
    const core::Clock& clock_;
    KeyStore<Bucket> store_;
};

} // namespace rl::limit
