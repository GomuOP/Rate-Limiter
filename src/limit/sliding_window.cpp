#include "limit/sliding_window.hpp"

#include <algorithm>
#include <cmath>

namespace rl::limit {

SlidingWindowLimiter::SlidingWindowLimiter(std::int64_t limit,
                                           std::chrono::milliseconds window,
                                           const core::Clock& clock,
                                           std::size_t max_keys,
                                           core::Duration idle_ttl)
    : limit_(limit), window_ms_(window.count()), clock_(clock), store_(max_keys, idle_ttl, clock) {
}

Decision SlidingWindowLimiter::check(std::string_view key, std::int64_t cost) {
    const auto now = clock_.now();
    const std::int64_t now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const std::int64_t idx = now_ms / window_ms_;
    const std::int64_t pos_ms = now_ms % window_ms_;

    Window& w = store_.get_or_create(key, [&] { return Window{idx, 0, 0}; });

    // Roll the window forward to `idx`.
    if (w.cur_idx == idx) {
        // same window
    } else if (w.cur_idx == idx - 1) {
        w.prev_count = w.cur_count;
        w.cur_count = 0;
        w.cur_idx = idx;
    } else {
        w.prev_count = 0;
        w.cur_count = 0;
        w.cur_idx = idx;
    }

    const double weight_prev =
        static_cast<double>(window_ms_ - pos_ms) / static_cast<double>(window_ms_);
    const double estimate =
        static_cast<double>(w.prev_count) * weight_prev + static_cast<double>(w.cur_count);

    Decision d;
    d.limit = limit_;

    if (estimate + static_cast<double>(cost) <= static_cast<double>(limit_)) {
        w.cur_count += cost;
        d.allowed = true;
    } else {
        d.allowed = false;

        // Estimate time until the rolling count would admit `cost` again.
        // While inside the current window the estimate only falls as the
        // previous window's weight decays; solve for that, else wait for the
        // window boundary.
        std::int64_t wait_ms = window_ms_ - pos_ms; // boundary fallback
        if (w.prev_count > 0) {
            const double target =
                static_cast<double>(limit_ - cost) - static_cast<double>(w.cur_count);
            if (target >= 0.0) {
                const double frac = 1.0 - target / static_cast<double>(w.prev_count);
                const double solved_pos = frac * static_cast<double>(window_ms_);
                const double t = solved_pos - static_cast<double>(pos_ms);
                if (t >= 0.0 && t < static_cast<double>(wait_ms)) {
                    wait_ms = static_cast<std::int64_t>(std::ceil(t));
                }
            }
        }
        d.retry_after = std::chrono::milliseconds{std::max<std::int64_t>(1, wait_ms)};
    }

    const double estimate_after = estimate + (d.allowed ? static_cast<double>(cost) : 0.0);
    d.remaining =
        std::max<std::int64_t>(0, limit_ - static_cast<std::int64_t>(std::ceil(estimate_after)));
    d.reset = std::chrono::milliseconds{2 * window_ms_ - pos_ms};
    return d;
}

} // namespace rl::limit
