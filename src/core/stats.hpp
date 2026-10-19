#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace rl::core {

/// Monotonic event counter, safe to bump from any thread (design 0.20).
///
/// Relaxed ordering is enough: fetch_add is an atomic read-modify-write at
/// every memory order, so no increment is lost. Relaxed only gives up ordering
/// against *other* memory, and no code uses a counter to publish other data -
/// readers take an approximate snapshot. Only inc/get are exposed so a
/// seq_cst `++` cannot creep in.
class Counter {
  public:
    void inc(std::uint64_t n = 1) noexcept { v_.fetch_add(n, std::memory_order_relaxed); }
    std::uint64_t get() const noexcept { return v_.load(std::memory_order_relaxed); }

  private:
    std::atomic<std::uint64_t> v_{0};
};

/// Service-wide counters, shared by every worker thread.
struct Stats {
    Counter requests;       // total HTTP requests handled
    Counter checks;         // requests routed to /check
    Counter allowed;        // /check decisions that allowed
    Counter denied;         // /check decisions that returned 429
    Counter bad_requests;   // 400s (missing key, malformed request)
    Counter not_found;      // 404s
    Counter config_changes; // successful POST /config swaps
    Counter conns_accepted;
    Counter conns_closed;
    Counter sweeps_removed;   // key-store idle-sweep removals
    Counter redis_errors;     // redis call failures (timeout / conn / reply)
    Counter redis_failopen;   // requests allowed because redis was down (fail-open)
    Counter redis_failclosed; // requests denied because redis was down (fail-closed)

    /// `key_count` and `evictions` come from the live limiter, not from here.
    nlohmann::json
    to_json(std::size_t key_count, std::uint64_t evictions, std::int64_t uptime_ms) const;
};

} // namespace rl::core
