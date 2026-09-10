#include "limit/token_bucket.hpp"

#include <algorithm>
#include <cmath>

namespace rl::limit {

namespace {

// Absorbs binary floating-point slop so that e.g. a 100ms refill at 10 tok/s
// counts as a full token rather than 0.999999999.
constexpr double kEps = 1e-9;

std::chrono::milliseconds ceil_ms(double seconds) {
    if (seconds <= 0.0) {
        return std::chrono::milliseconds{0};
    }
    return std::chrono::milliseconds{static_cast<std::int64_t>(std::ceil(seconds * 1000.0 - kEps))};
}

} // namespace

TokenBucketLimiter::TokenBucketLimiter(double rate,
                                       std::int64_t burst,
                                       const core::Clock& clock,
                                       std::size_t max_keys,
                                       core::Duration idle_ttl)
    : rate_(rate), burst_(static_cast<double>(burst)), clock_(clock),
      store_(max_keys, idle_ttl, clock) {
}

Decision TokenBucketLimiter::check(std::string_view key, std::int64_t cost) {
    const auto now = clock_.now();

    Bucket& b = store_.get_or_create(key, [&] { return Bucket{burst_, now}; });

    // Lazy refill: fold elapsed time into the token count.
    const double elapsed_s = std::max(0.0, std::chrono::duration<double>(now - b.last).count());
    b.tokens = std::min(burst_, b.tokens + elapsed_s * rate_);
    b.last = now;

    const double want = static_cast<double>(cost);
    Decision d;
    d.limit = static_cast<std::int64_t>(burst_);

    if (b.tokens >= want - kEps) {
        b.tokens -= want;
        d.allowed = true;
    } else {
        d.allowed = false;
        d.retry_after = ceil_ms((want - b.tokens) / rate_);
    }

    d.remaining = static_cast<std::int64_t>(std::floor(std::max(0.0, b.tokens) + kEps));
    d.reset = ceil_ms((burst_ - b.tokens) / rate_);
    return d;
}

} // namespace rl::limit
