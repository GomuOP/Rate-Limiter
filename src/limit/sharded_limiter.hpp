#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "limit/rate_limiter.hpp"

namespace rl::limit {

/// Thread-safe limiter shared by every worker thread (design 0.20).
///
/// Lock striping: K shards, each = one mutex + one ordinary (single-threaded)
/// limiter. A key always maps to the same shard (hash(key) % K), so all
/// requests for that key go through one lock and one bucket. Different keys
/// usually land on different shards and run in parallel.
///
/// The inner limiters are NOT thread-safe on their own; the shard's mutex is
/// what makes it safe. Rule: never touch `shard.inner` without holding
/// `shard.mu`.
class ShardedLimiter final : public RateLimiter {
  public:
    /// Builds one fresh inner limiter. Called once per shard by the constructor.
    using Factory = std::function<std::unique_ptr<RateLimiter>()>;

    ShardedLimiter(std::size_t shard_count, const Factory& make_inner);

    Decision check(std::string_view key, std::int64_t cost = 1) override;
    std::size_t key_count() const override;
    std::size_t sweep_expired() override;
    std::size_t evictions() const override;

    std::size_t shard_count() const { return shards_.size(); }

  private:
    struct Shard {
        // `mutable` lets const functions (key_count, evictions) lock it too:
        // locking changes the mutex, but not the limiter's logical state.
        mutable std::mutex mu;
        std::unique_ptr<RateLimiter> inner; // guarded by mu
    };

    Shard& shard_for(std::string_view key);

    // std::mutex can't be copied or moved, so the vector is created at its
    // final size once (see the constructor) and never resized afterwards.
    std::vector<Shard> shards_;
};

} // namespace rl::limit
