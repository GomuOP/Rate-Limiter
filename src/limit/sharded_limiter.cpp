#include "limit/sharded_limiter.hpp"

#include <stdexcept>

namespace rl::limit {

ShardedLimiter::ShardedLimiter(std::size_t shard_count, const Factory& make_inner)
    : shards_(shard_count) {
    if (shard_count == 0) {
        throw std::invalid_argument("ShardedLimiter needs at least 1 shard");
    }
    for (auto& s : shards_) {
        s.inner = make_inner();
    }
}

ShardedLimiter::Shard& ShardedLimiter::shard_for(std::string_view key) {
    const std::size_t h = std::hash<std::string_view>{}(key);
    return shards_[h % shards_.size()];
}

Decision ShardedLimiter::check(std::string_view key, std::int64_t cost) {
    auto& s = shard_for(key);
    std::lock_guard<std::mutex> lock(s.mu);
    return s.inner->check(key, cost);
}

std::size_t ShardedLimiter::key_count() const {
    std::size_t tot = 0;
    for (auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s.mu);
        tot += s.inner->key_count();
    }
    return tot;
}

std::size_t ShardedLimiter::sweep_expired() {
    std::size_t tot = 0;
    for (auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s.mu);
        tot += s.inner->sweep_expired();
    }
    return tot;
}

std::size_t ShardedLimiter::evictions() const {
    std::size_t tot = 0;
    for (auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s.mu);
        tot += s.inner->evictions();
    }
    return tot;
}

} // namespace rl::limit
