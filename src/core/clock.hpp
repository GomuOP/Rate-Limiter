#pragma once

#include <chrono>

namespace rl::core {

using Clock_t = std::chrono::steady_clock;
using TimePoint = Clock_t::time_point;
using Duration = std::chrono::nanoseconds;

/// Injectable monotonic clock. Every limiter takes a `Clock&` so that refill and
/// window behaviour can be asserted exactly in tests (design 0.3).
class Clock {
  public:
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;
};

/// Production clock: wraps std::chrono::steady_clock.
class SystemClock final : public Clock {
  public:
    TimePoint now() const override { return Clock_t::now(); }
};

/// Test clock: time only moves when the test moves it.
class ManualClock final : public Clock {
  public:
    explicit ManualClock(TimePoint start = TimePoint{}) : now_(start) {}

    TimePoint now() const override { return now_; }

    void advance(Duration d) { now_ += d; }
    template <class Rep, class Period> void advance(std::chrono::duration<Rep, Period> d) {
        now_ += std::chrono::duration_cast<Duration>(d);
    }
    void set(TimePoint t) { now_ = t; }

  private:
    TimePoint now_;
};

} // namespace rl::core
