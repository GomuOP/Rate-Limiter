#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/stats.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "limit/key_extractor.hpp"
#include "limit/rate_limiter.hpp"
#include "net/connection.hpp"
#include "net/event_loop.hpp"
#include "net/listener.hpp"

namespace rl::http {

/// Builds a fresh limiter from a limiter config (design 0.19). Supplied by
/// main() so the server can rebuild the limiter for a live /config change
/// without knowing about concrete algorithms or the clock.
using LimiterFactory =
    std::function<std::unique_ptr<limit::RateLimiter>(const core::LimiterConfig&)>;

/// The HTTP service (design 0.1, 0.17, 0.19, 0.20): one epoll loop, one
/// listener, a map of live connections, and the routes GET / (dashboard),
/// GET /check, GET /healthz, GET /stats, GET|POST /config.
///
/// With server.threads = N, main() builds N Servers and runs each on its own
/// thread. They share `stats` and, in local mode, one thread-safe limiter;
/// everything else (loop, listener, connections) belongs to one Server.
class Server {
  public:
    /// `limiter` is the initial limiter, possibly shared with other Servers;
    /// `make_limiter` rebuilds it when POST /config arrives (threads = 1
    /// only). `run_sweeper` is true on exactly one Server so a shared limiter
    /// is swept once per interval, not once per thread.
    Server(const core::Config& cfg,
           std::shared_ptr<limit::RateLimiter> limiter,
           LimiterFactory make_limiter,
           core::Stats& stats,
           bool run_sweeper = true);

    /// Runs the event loop on the calling thread until stop().
    void run();
    /// Safe to call from any thread.
    void stop() { loop_.stop(); }

    net::EventLoop& loop() { return loop_; }
    std::size_t connection_count() const { return conns_.size(); }

  private:
    void on_accept(net::Fd fd, std::string peer_ip);
    void handle(const Request& req, Response& resp);
    void handle_check(const Request& req, Response& resp);
    void handle_config(const Request& req, Response& resp);
    void reap_idle();

    const core::Config& cfg_;
    std::shared_ptr<limit::RateLimiter> limiter_;
    LimiterFactory make_limiter_;
    core::LimiterConfig live_limiter_cfg_;
    limit::KeyExtractor extractor_;
    core::Stats& stats_;
    std::uint64_t retired_evictions_ = 0; // evictions from limiters replaced by /config
    bool run_sweeper_;
    core::TimePoint start_;

    net::EventLoop loop_;
    net::Connection::RequestHandler handler_;
    net::Listener listener_;
    std::unordered_map<int, std::unique_ptr<net::Connection>> conns_;
};

} // namespace rl::http
