#include "core/stats.hpp"

namespace rl::core {

nlohmann::json
Stats::to_json(std::size_t key_count, std::uint64_t evictions, std::int64_t uptime_ms) const {
    return nlohmann::json{
        {"uptime_ms", uptime_ms},
        {"requests", requests.get()},
        {"checks", checks.get()},
        {"allowed", allowed.get()},
        {"denied", denied.get()},
        {"bad_requests", bad_requests.get()},
        {"not_found", not_found.get()},
        {"config_changes", config_changes.get()},
        {"conns_accepted", conns_accepted.get()},
        {"conns_closed", conns_closed.get()},
        {"key_count", key_count},
        {"evictions", evictions},
        {"sweeps_removed", sweeps_removed.get()},
        {"redis_errors", redis_errors.get()},
        {"redis_failopen", redis_failopen.get()},
        {"redis_failclosed", redis_failclosed.get()},
    };
}

} // namespace rl::core
