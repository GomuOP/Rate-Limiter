#include "core/stats.hpp"

namespace rl::core {

nlohmann::json Stats::to_json(std::size_t key_count, std::int64_t uptime_ms) const {
    return nlohmann::json{
        {"uptime_ms", uptime_ms},
        {"requests", requests},
        {"checks", checks},
        {"allowed", allowed},
        {"denied", denied},
        {"bad_requests", bad_requests},
        {"not_found", not_found},
        {"conns_accepted", conns_accepted},
        {"conns_closed", conns_closed},
        {"key_count", key_count},
        {"evictions", evictions},
        {"sweeps_removed", sweeps_removed},
        {"redis_errors", redis_errors},
        {"redis_failopen", redis_failopen},
        {"redis_failclosed", redis_failclosed},
    };
}

} // namespace rl::core
