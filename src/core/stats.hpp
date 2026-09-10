#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace rl::core {

/// Plain counters — the service is single-threaded (design 0.1), so no atomics.
struct Stats {
    std::uint64_t requests = 0;       // total HTTP requests handled
    std::uint64_t checks = 0;         // requests routed to /check
    std::uint64_t allowed = 0;        // /check decisions that allowed
    std::uint64_t denied = 0;         // /check decisions that returned 429
    std::uint64_t bad_requests = 0;   // 400s (missing key, malformed request)
    std::uint64_t not_found = 0;      // 404s
    std::uint64_t config_changes = 0; // successful POST /config swaps
    std::uint64_t conns_accepted = 0;
    std::uint64_t conns_closed = 0;
    std::uint64_t evictions = 0;        // key-store LRU evictions
    std::uint64_t sweeps_removed = 0;   // key-store idle-sweep removals
    std::uint64_t redis_errors = 0;     // redis call failures (timeout / conn / reply)
    std::uint64_t redis_failopen = 0;   // requests allowed because redis was down (fail-open)
    std::uint64_t redis_failclosed = 0; // requests denied because redis was down (fail-closed)

    nlohmann::json to_json(std::size_t key_count, std::int64_t uptime_ms) const;
};

} // namespace rl::core
