#pragma once

#include <cstdint>
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

/// The HTTP service (design 0.1, 0.17): one epoll loop, one listener, a map of
/// live connections, and three routes — GET /check, GET /healthz, GET /stats.
class Server {
  public:
    Server(const core::Config& cfg, limit::RateLimiter& limiter, core::Stats& stats);

    void run();
    void stop() { loop_.stop(); }

    net::EventLoop& loop() { return loop_; }
    std::size_t connection_count() const { return conns_.size(); }

  private:
    void on_accept(net::Fd fd, std::string peer_ip);
    void handle(const Request& req, Response& resp);
    void handle_check(const Request& req, Response& resp);
    void reap_idle();

    const core::Config& cfg_;
    limit::RateLimiter& limiter_;
    limit::KeyExtractor extractor_;
    core::Stats& stats_;
    core::TimePoint start_;

    net::EventLoop loop_;
    net::Connection::RequestHandler handler_;
    net::Listener listener_;
    std::unordered_map<int, std::unique_ptr<net::Connection>> conns_;
};

} // namespace rl::http
