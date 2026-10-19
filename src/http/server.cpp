#include "http/server.hpp"

#include <sys/epoll.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "http/query.hpp"
#include "rl/dashboard.inc" // provides rl::http::kDashboardHtml

namespace rl::http {

namespace {
std::int64_t retry_after_seconds(std::chrono::milliseconds ms) {
    auto s = static_cast<std::int64_t>(std::ceil(ms.count() / 1000.0));
    return s < 1 ? 1 : s;
}

nlohmann::json limiter_cfg_json(const core::LimiterConfig& lc) {
    return nlohmann::json{
        {"algo",
         lc.algo == core::LimiterConfig::Algo::token_bucket ? "token_bucket" : "sliding_window"},
        {"rate", lc.rate},
        {"burst", lc.burst},
        {"limit", lc.limit},
        {"window_ms", lc.window.count()},
    };
}

/// Parse a whole `std::int64_t` from a decoded query value; nullopt on any
/// trailing garbage or overflow.
std::optional<std::int64_t> parse_i64(const std::string& s) {
    std::int64_t out = 0;
    const char* begin = s.data();
    const char* end = begin + s.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return out;
}

/// Parse a finite `double` from a decoded query value; nullopt on trailing
/// garbage, empty input, or a non-finite result.
std::optional<double> parse_double(const std::string& s) {
    if (s.empty()) {
        return std::nullopt;
    }
    errno = 0;
    char* endp = nullptr;
    double v = std::strtod(s.c_str(), &endp);
    if (endp != s.c_str() + s.size() || errno == ERANGE || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}
} // namespace

Server::Server(const core::Config& cfg,
               std::unique_ptr<limit::RateLimiter> limiter,
               LimiterFactory make_limiter,
               core::Stats& stats)
    : cfg_(cfg), limiter_(std::move(limiter)), make_limiter_(std::move(make_limiter)),
      live_limiter_cfg_(cfg.limiter), extractor_(cfg.key_extractor), stats_(stats),
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

    if (req.path == "/config") {
        handle_config(req, resp);
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
        stats_.evictions = retired_evictions_ + limiter_->evictions();
        resp.set_status(200);
        resp.set_json(stats_.to_json(limiter_->key_count(), uptime));
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

    const limit::Decision d = limiter_->check(key.key, 1);

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

// GET  /config  -> the limiter config currently in effect.
// POST /config?algo=&rate=&burst=&limit=&window_ms=  -> merge the given fields
// onto the live config, rebuild the limiter, and swap it in. Any field left
// out keeps its current value. A rebuild starts from an empty key store, so
// existing buckets/windows are cleared (design 0.19). Local mode only.
void Server::handle_config(const Request& req, Response& resp) {
    if (req.method == "GET" || req.method == "HEAD") {
        resp.set_status(200);
        resp.set_json(limiter_cfg_json(live_limiter_cfg_));
        return;
    }
    if (req.method != "POST") {
        resp.set_status(405);
        resp.add_header("Allow", "GET, POST");
        resp.set_json({{"error", "method not allowed"}});
        return;
    }

    if (cfg_.mode != core::Config::Mode::local) {
        resp.set_status(409);
        resp.set_json({{"error", "live limiter reconfiguration is only supported in local mode"}});
        return;
    }

    auto bad = [&](const std::string& msg) {
        ++stats_.bad_requests;
        resp.set_status(400);
        resp.set_json({{"error", msg}});
    };

    core::LimiterConfig next = live_limiter_cfg_;
    bool changed = false;

    if (auto v = query_param(req.query, "algo")) {
        if (*v == "token_bucket") {
            next.algo = core::LimiterConfig::Algo::token_bucket;
        } else if (*v == "sliding_window") {
            next.algo = core::LimiterConfig::Algo::sliding_window;
        } else {
            return bad("algo must be 'token_bucket' or 'sliding_window'");
        }
        changed = true;
    }
    if (auto v = query_param(req.query, "rate")) {
        auto d = parse_double(*v);
        if (!d) {
            return bad("rate must be a number");
        }
        next.rate = *d;
        changed = true;
    }
    if (auto v = query_param(req.query, "burst")) {
        auto n = parse_i64(*v);
        if (!n) {
            return bad("burst must be an integer");
        }
        next.burst = *n;
        changed = true;
    }
    if (auto v = query_param(req.query, "limit")) {
        auto n = parse_i64(*v);
        if (!n) {
            return bad("limit must be an integer");
        }
        next.limit = *n;
        changed = true;
    }
    if (auto v = query_param(req.query, "window_ms")) {
        auto n = parse_i64(*v);
        if (!n) {
            return bad("window_ms must be an integer");
        }
        next.window = std::chrono::milliseconds{*n};
        changed = true;
    }

    if (!changed) {
        return bad("no recognised parameters (algo, rate, burst, limit, window_ms)");
    }

    try {
        next.validate();
    } catch (const std::exception& e) {
        return bad(e.what());
    }

    auto rebuilt = make_limiter_(next);
    if (!rebuilt) {
        resp.set_status(500);
        resp.set_json({{"error", "limiter rebuild failed"}});
        return;
    }

    retired_evictions_ += limiter_->evictions();
    limiter_ = std::move(rebuilt);
    live_limiter_cfg_ = next;
    ++stats_.config_changes;

    nlohmann::json body = limiter_cfg_json(live_limiter_cfg_);
    body["note"] = "limiter replaced; key store reset, counters unchanged";
    resp.set_status(200);
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
        [this] { stats_.sweeps_removed += limiter_->sweep_expired(); }, /*repeat=*/true,
        cfg_.key_store.sweep_interval);

    auto reap_every = cfg_.server.idle_timeout / 2;
    if (reap_every < std::chrono::milliseconds(1000)) {
        reap_every = std::chrono::milliseconds(1000);
    }
    loop_.add_timer(reap_every, [this] { reap_idle(); }, /*repeat=*/true, reap_every);

    loop_.run();
}

} // namespace rl::http
