#include "http/server.hpp"

#include <sys/epoll.h>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "rl/dashboard.inc" // provides rl::http::kDashboardHtml

namespace rl::http {

namespace {
std::int64_t retry_after_seconds(std::chrono::milliseconds ms) {
    auto s = static_cast<std::int64_t>(std::ceil(ms.count() / 1000.0));
    return s < 1 ? 1 : s;
}
} // namespace

Server::Server(const core::Config& cfg, limit::RateLimiter& limiter, core::Stats& stats)
    : cfg_(cfg), limiter_(limiter), extractor_(cfg.key_extractor), stats_(stats),
      start_(core::Clock_t::now()), loop_(),
      handler_([this](const Request& req, Response& resp) { handle(req, resp); }),
      listener_(loop_,
                cfg.server.host,
                cfg.server.port,
                cfg.server.backlog,
                [this](net::Fd fd, std::string ip) { on_accept(std::move(fd), std::move(ip)); }) {
}

void Server::on_accept(net::Fd fd, std::string peer_ip) {
    ++stats_.conns_accepted;
    const int fd_num = fd.get();
    auto conn = std::make_unique<net::Connection>(loop_, std::move(fd), std::move(peer_ip),
                                                  cfg_.server, handler_, [this](int f) {
                                                      if (conns_.erase(f) > 0) {
                                                          ++stats_.conns_closed;
                                                      }
                                                  });
    conns_.emplace(fd_num, std::move(conn));
}

void Server::handle(const Request& req, Response& resp) {
    ++stats_.requests;

    if (req.path == "/" || req.path == "/dashboard") {
        resp.set_status(200);
        resp.set_html(kDashboardHtml);
        return;
    }

    if (req.path == "/check") {
        if (req.method != "GET" && req.method != "HEAD") {
            resp.set_status(405);
            resp.add_header("Allow", "GET, HEAD");
            resp.set_json({{"error", "method not allowed"}});
            return;
        }
        handle_check(req, resp);
        return;
    }

    if (req.path == "/healthz") {
        resp.set_status(200);
        resp.set_text("ok\n");
        return;
    }

    if (req.path == "/stats") {
        const auto uptime =
            std::chrono::duration_cast<std::chrono::milliseconds>(core::Clock_t::now() - start_)
                .count();
        resp.set_status(200);
        resp.set_json(stats_.to_json(limiter_.key_count(), uptime));
        return;
    }

    ++stats_.not_found;
    resp.set_status(404);
    resp.set_json({{"error", "not found"}});
}

void Server::handle_check(const Request& req, Response& resp) {
    ++stats_.checks;

    auto key = extractor_.extract(req);
    if (!key.ok) {
        ++stats_.bad_requests;
        resp.set_status(400);
        resp.set_json({{"error", key.error}});
        return;
    }

    const limit::Decision d = limiter_.check(key.key, 1);

    nlohmann::json body{
        {"allowed", d.allowed},
        {"limit", d.limit},
        {"remaining", d.remaining},
        {"reset_ms", d.reset.count()},
    };
    resp.add_header("X-RateLimit-Limit", std::to_string(d.limit));
    resp.add_header("X-RateLimit-Remaining", std::to_string(d.remaining));

    if (d.allowed) {
        ++stats_.allowed;
        resp.set_status(200);
    } else {
        ++stats_.denied;
        resp.set_status(429);
        resp.add_header("Retry-After", std::to_string(retry_after_seconds(d.retry_after)));
        body["retry_after_ms"] = d.retry_after.count();
    }
    resp.set_json(body);
}

void Server::reap_idle() {
    const auto now = core::Clock_t::now();
    std::vector<net::Connection*> stale;
    for (auto& [fd, conn] : conns_) {
        if (now - conn->last_activity() > cfg_.server.idle_timeout) {
            stale.push_back(conn.get());
        }
    }
    for (auto* c : stale) {
        c->close(); // defers erase from conns_
    }
}

void Server::run() {
    start_ = core::Clock_t::now();

    loop_.add_timer(
        cfg_.key_store.sweep_interval,
        [this] { stats_.sweeps_removed += limiter_.sweep_expired(); }, /*repeat=*/true,
        cfg_.key_store.sweep_interval);

    auto reap_every = cfg_.server.idle_timeout / 2;
    if (reap_every < std::chrono::milliseconds(1000)) {
        reap_every = std::chrono::milliseconds(1000);
    }
    loop_.add_timer(reap_every, [this] { reap_idle(); }, /*repeat=*/true, reap_every);

    loop_.run();
}

} // namespace rl::http
