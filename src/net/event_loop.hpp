#pragma once

#include <sys/epoll.h>

#include <csignal>
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
    void stop() { stop_flag_ = 1; } // safe to call from a signal handler

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
    std::unordered_map<int, IoCallback> cbs_;
    std::priority_queue<Timer, std::vector<Timer>, Later> timers_;
    std::unordered_set<TimerId> cancelled_;
    std::vector<std::function<void()>> deferred_;
    std::vector<epoll_event> events_;
    TimerId next_timer_id_ = 1;
    volatile std::sig_atomic_t stop_flag_ = 0;
    bool running_ = false;
};

} // namespace rl::net
