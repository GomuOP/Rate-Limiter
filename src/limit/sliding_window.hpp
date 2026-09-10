#pragma once

#include <cstdint>

#include "core/clock.hpp"
#include "limit/key_store.hpp"
#include "limit/rate_limiter.hpp"

namespace rl::limit {

/// Sliding-window *counter* (design 0.2): keeps the count for the current fixed
/// window and the previous one, and estimates the rolling count as
///   estimate = prev * (fraction of the previous window still in view) + cur
/// This removes the 2x boundary burst of a plain fixed window for one multiply,
/// at the cost of a bounded approximation error (documented in the README).
class SlidingWindowLimiter final : public RateLimiter {
  public:
    SlidingWindowLimiter(std::int64_t limit,
                         std::chrono::milliseconds window,
                         const core::Clock& clock,
                         std::size_t max_keys,
                         core::Duration idle_ttl);

    Decision check(std::string_view key, std::int64_t cost = 1) override;
    std::size_t key_count() const override { return store_.size(); }
    std::size_t sweep_expired() override { return store_.sweep_expired(); }

    std::size_t evictions() const { return store_.evictions(); }

  private:
    struct Window {
        std::int64_t cur_idx;
        std::int64_t cur_count;
        std::int64_t prev_count;
    };

    std::int64_t limit_;
    std::int64_t window_ms_;
    const core::Clock& clock_;
    KeyStore<Window> store_;
};

} // namespace rl::limit
