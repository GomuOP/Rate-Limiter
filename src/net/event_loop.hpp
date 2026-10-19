#pragma once

#include <sys/epoll.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/clock.hpp"
#include "net/fd.hpp"

namespace rl::net {

/// Single-threaded epoll reactor (design 0.1). Level-triggered (Q-P1.1 default):
/// readiness stays asserted until the fd is drained, so a handler that reads
/// once and returns is still correct; handlers here drain until EAGAIN anyway
/// for throughput.
///
/// Everything except stop() must be called from the thread running run().
/// With several worker threads each owns one loop (design 0.20).
class EventLoop {
  public:
    using IoCallback = std::function<void(std::uint32_t events)>;
    using TimerId = std::uint64_t;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void add(int fd, std::uint32_t events, IoCallback cb);
    void mod(int fd, std::uint32_t events);
    void del(int fd);

    TimerId add_timer(core::Duration after,
                      std::function<void()> cb,
                      bool repeat = false,
                      core::Duration interval = core::Duration::zero());
    void cancel_timer(TimerId id);

    /// Queue work to run once, right after the current epoll dispatch batch.
    /// Used so a connection can close itself without freeing an fd that later
    /// events in the same batch still refer to.
    void defer(std::function<void()> fn);

    void run();

    /// Make run() return. Safe from any thread (and async-signal-safe): sets
    /// the flag, then writes the eventfd so a loop blocked in epoll_wait with
    /// nothing else to do wakes up and sees it.
    void stop();

  private:
    int next_timeout_ms(core::TimePoint now) const;
    void fire_due_timers(core::TimePoint now);
    void drain_deferred();

    struct Timer {
        core::TimePoint when;
        TimerId id;
        std::function<void()> cb;
        bool repeat;
        core::Duration interval;
    };
    struct Later {
        bool operator()(const Timer& a, const Timer& b) const { return a.when > b.when; }
    };

    Fd epfd_;
    Fd wakefd_; // eventfd registered in epfd_; written by stop()
    std::unordered_map<int, IoCallback> cbs_;
    std::priority_queue<Timer, std::vector<Timer>, Later> timers_;
    std::unordered_set<TimerId> cancelled_;
    std::vector<std::function<void()>> deferred_;
    std::vector<epoll_event> events_;
    TimerId next_timer_id_ = 1;
    // Relaxed is enough: the flag publishes no other data, and the eventfd
    // write is what guarantees the loop wakes up to re-read it.
    std::atomic<bool> stop_flag_{false};
    bool running_ = false;
};

} // namespace rl::net
