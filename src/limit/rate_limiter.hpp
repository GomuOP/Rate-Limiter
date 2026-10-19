#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rl::limit {

/// Result of a single rate-limit check.
struct Decision {
    bool allowed = false;
    std::int64_t limit = 0;                   // configured capacity (burst, or window limit)
    std::int64_t remaining = 0;               // whole units left after this call
    std::chrono::milliseconds retry_after{0}; // >0 hint when denied, 0 when allowed
    std::chrono::milliseconds reset{0};       // time until fully replenished / window clear
};

/// Common interface for every limiter (design 0.2). Implementations are driven by
/// a single thread and are not internally synchronised.
class RateLimiter {
  public:
    virtual ~RateLimiter() = default;

    /// Charge `cost` units against `key` and return the decision.
    virtual Decision check(std::string_view key, std::int64_t cost = 1) = 0;

    /// Number of distinct keys currently held (0 for backends that don't track).
    virtual std::size_t key_count() const = 0;

    /// Drop entries idle longer than the configured TTL. Called from the event
    /// loop's periodic timer. Returns the number removed.
    virtual std::size_t sweep_expired() = 0;

    /// Entries evicted by the LRU bound since construction (0 for backends
    /// that don't track).
    virtual std::size_t evictions() const = 0;
};

} // namespace rl::limit
