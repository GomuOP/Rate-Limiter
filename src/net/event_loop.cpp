#include "net/event_loop.hpp"

#include <sys/epoll.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <system_error>

namespace rl::net {

EventLoop::EventLoop() : epfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(1024) {
    if (!epfd_) {
        throw std::system_error(errno, std::system_category(), "epoll_create1");
    }
}

EventLoop::~EventLoop() = default;

void EventLoop::add(int fd, std::uint32_t events, IoCallback cb) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl ADD");
    }
    cbs_[fd] = std::move(cb);
}

void EventLoop::mod(int fd, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_.get(), EPOLL_CTL_MOD, fd, &ev) != 0) {
        throw std::system_error(errno, std::system_category(), "epoll_ctl MOD");
    }
}

void EventLoop::del(int fd) {
    ::epoll_ctl(epfd_.get(), EPOLL_CTL_DEL, fd, nullptr); // ignore errors (fd may be gone)
    cbs_.erase(fd);
}

EventLoop::TimerId EventLoop::add_timer(core::Duration after,
                                        std::function<void()> cb,
                                        bool repeat,
                                        core::Duration interval) {
    const TimerId id = next_timer_id_++;
    timers_.push(Timer{core::Clock_t::now() + after, id, std::move(cb), repeat,
                       repeat && interval == core::Duration::zero() ? after : interval});
    return id;
}

void EventLoop::cancel_timer(TimerId id) {
    cancelled_.insert(id);
}

void EventLoop::defer(std::function<void()> fn) {
    deferred_.push_back(std::move(fn));
}

int EventLoop::next_timeout_ms(core::TimePoint now) const {
    if (!deferred_.empty()) {
        return 0;
    }
    if (timers_.empty()) {
        return -1; // block until an fd is ready
    }
    auto delta = timers_.top().when - now;
    if (delta <= core::Duration::zero()) {
        return 0;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    return static_cast<int>(std::min<std::int64_t>(ms + 1, 3600'000));
}

void EventLoop::fire_due_timers(core::TimePoint now) {
    while (!timers_.empty() && timers_.top().when <= now) {
        Timer t = std::move(const_cast<Timer&>(timers_.top()));
        timers_.pop();
        if (auto c = cancelled_.find(t.id); c != cancelled_.end()) {
            cancelled_.erase(c);
            continue;
        }
        t.cb();
        if (t.repeat) {
            t.when += t.interval;
            if (t.when <= now) {
                t.when = now + t.interval; // fell behind; don't stampede
            }
            timers_.push(std::move(t));
        }
    }
}

void EventLoop::drain_deferred() {
    // Callbacks may enqueue more deferred work; run to a fixed point.
    while (!deferred_.empty()) {
        std::vector<std::function<void()>> batch;
        batch.swap(deferred_);
        for (auto& fn : batch) {
            fn();
        }
    }
}

void EventLoop::run() {
    running_ = true;
    while (running_ && !stop_flag_) {
        const auto now = core::Clock_t::now();
        const int timeout = next_timeout_ms(now);

        int n =
            ::epoll_wait(epfd_.get(), events_.data(), static_cast<int>(events_.size()), timeout);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // re-check stop_flag_ / timers
            }
            throw std::system_error(errno, std::system_category(), "epoll_wait");
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events_[i].data.fd;
            auto it = cbs_.find(fd);
            if (it == cbs_.end()) {
                continue; // handler was removed earlier in this same batch
            }
            it->second(events_[i].events);
        }

        drain_deferred();
        fire_due_timers(core::Clock_t::now());

        if (n == static_cast<int>(events_.size())) {
            events_.resize(events_.size() * 2); // batch was full; grow for next time
        }
    }
    running_ = false;
}

} // namespace rl::net
