#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/stats.hpp"
#include "http/server.hpp"
#include "limit/rate_limiter.hpp"
#include "limit/sliding_window.hpp"
#include "limit/token_bucket.hpp"
#include "redis/redis_rate_limiter.hpp"

namespace {

rl::http::Server* g_server = nullptr;

void on_signal(int) {
    if (g_server != nullptr) {
        g_server->stop(); // sets a flag; epoll_wait returns via EINTR
    }
}

void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [options]\n"
              << "  --config <path>        JSON config file\n"
              << "  --listen <host:port>   override server.host / server.port\n"
              << "  --mode <local|redis>   override mode\n"
              << "  --redis <host:port>    override redis.host / redis.port\n"
              << "  -h, --help\n";
}

/// Split "host:port" on the last ':'. An empty host keeps the existing value.
void apply_hostport(std::string_view hp, std::string& host, std::uint16_t& port) {
    auto colon = hp.rfind(':');
    if (colon == std::string_view::npos) {
        throw std::runtime_error("expected host:port, got '" + std::string(hp) + "'");
    }
    auto h = hp.substr(0, colon);
    auto p = hp.substr(colon + 1);
    if (!h.empty()) {
        host.assign(h);
    }
    port = static_cast<std::uint16_t>(std::stoi(std::string(p)));
}

// Build a limiter for `lc`, taking non-algorithm settings (mode, key-store
// bounds, redis endpoint) from `base`. Called once at startup and again by the
// server for every accepted POST /config (design 0.19).
std::unique_ptr<rl::limit::RateLimiter> make_limiter(const rl::core::Config& base,
                                                     const rl::core::LimiterConfig& lc,
                                                     const rl::core::Clock& clock,
                                                     rl::core::Stats& stats) {
    using rl::core::Config;
    using rl::core::LimiterConfig;

    if (base.mode == Config::Mode::redis) {
        return std::make_unique<rl::redis::RedisRateLimiter>(base.redis, lc.rate, lc.burst, stats);
    }
    if (lc.algo == LimiterConfig::Algo::token_bucket) {
        return std::make_unique<rl::limit::TokenBucketLimiter>(
            lc.rate, lc.burst, clock, base.key_store.max_keys, base.key_store.idle_ttl);
    }
    return std::make_unique<rl::limit::SlidingWindowLimiter>(
        lc.limit, lc.window, clock, base.key_store.max_keys, base.key_store.idle_ttl);
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::optional<std::string> listen_override, redis_override, mode_override;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need_arg = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs an argument\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--config") {
            config_path = need_arg();
        } else if (a == "--listen") {
            listen_override = need_arg();
        } else if (a == "--redis") {
            redis_override = need_arg();
        } else if (a == "--mode") {
            mode_override = need_arg();
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    rl::core::Config cfg;
    try {
        if (!config_path.empty()) {
            cfg = rl::core::Config::from_json_file(config_path);
        }
        if (mode_override) {
            cfg.mode = (*mode_override == "redis") ? rl::core::Config::Mode::redis
                                                   : rl::core::Config::Mode::local;
        }
        if (listen_override) {
            apply_hostport(*listen_override, cfg.server.host, cfg.server.port);
        }
        if (redis_override) {
            apply_hostport(*redis_override, cfg.redis.host, cfg.redis.port);
        }
        cfg.validate();
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << "\n";
        return 1;
    }

    rl::core::SystemClock clock;
    rl::core::Stats stats;
    rl::http::LimiterFactory limiter_factory = [&cfg, &clock,
                                                &stats](const rl::core::LimiterConfig& lc) {
        return make_limiter(cfg, lc, clock, stats);
    };
    auto limiter = limiter_factory(cfg.limiter);

    std::signal(SIGPIPE, SIG_IGN);

    try {
        rl::http::Server server(cfg, std::move(limiter), limiter_factory, stats);
        g_server = &server;
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        const char* mode = (cfg.mode == rl::core::Config::Mode::redis) ? "redis" : "local";
        std::cerr << "ratelimiter listening on " << cfg.server.host << ":" << cfg.server.port
                  << " mode=" << mode << "\n";
        server.run();
        g_server = nullptr;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "ratelimiter stopped\n";
    return 0;
}
