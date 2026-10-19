#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "core/stats.hpp"
#include "http/server.hpp"
#include "limit/rate_limiter.hpp"
#include "limit/sharded_limiter.hpp"
#include "limit/sliding_window.hpp"
#include "limit/token_bucket.hpp"
#include "redis/redis_rate_limiter.hpp"

namespace {

// Lock stripes for the shared local limiter (design 0.20). Many more stripes
// than threads, so two busy keys rarely share a lock; each stripe costs one
// mutex and one small key store.
constexpr std::size_t kShardCount = 64;

void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [options]\n"
              << "  --config <path>        JSON config file\n"
              << "  --listen <host:port>   override server.host / server.port\n"
              << "  --mode <local|redis>   override mode\n"
              << "  --redis <host:port>    override redis.host / redis.port\n"
              << "  --threads <n>          override server.threads\n"
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

/// Parse a whole decimal int; throws on trailing garbage ("4x") or overflow.
int parse_int(std::string_view s, const char* what) {
    int out = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        throw std::runtime_error(std::string(what) + " must be an integer, got '" + std::string(s) +
                                 "'");
    }
    return out;
}

/// One single-threaded local limiter holding at most `max_keys` keys.
std::unique_ptr<rl::limit::RateLimiter> make_local_limiter(const rl::core::Config& base,
                                                           const rl::core::LimiterConfig& lc,
                                                           const rl::core::Clock& clock,
                                                           std::size_t max_keys) {
    if (lc.algo == rl::core::LimiterConfig::Algo::token_bucket) {
        return std::make_unique<rl::limit::TokenBucketLimiter>(lc.rate, lc.burst, clock, max_keys,
                                                               base.key_store.idle_ttl);
    }
    return std::make_unique<rl::limit::SlidingWindowLimiter>(lc.limit, lc.window, clock, max_keys,
                                                             base.key_store.idle_ttl);
}

// Build a limiter for `lc`, taking non-algorithm settings (mode, key-store
// bounds, redis endpoint, thread count) from `base`. Called at startup (once
// per thread in redis mode) and by the server for every accepted POST /config
// (design 0.19, 0.20).
std::unique_ptr<rl::limit::RateLimiter> make_limiter(const rl::core::Config& base,
                                                     const rl::core::LimiterConfig& lc,
                                                     const rl::core::Clock& clock,
                                                     rl::core::Stats& stats) {
    if (base.mode == rl::core::Config::Mode::redis) {
        // hiredis contexts are not thread-safe: every worker gets its own.
        return std::make_unique<rl::redis::RedisRateLimiter>(base.redis, lc.rate, lc.burst, stats);
    }
    if (base.server.threads == 1) {
        return make_local_limiter(base, lc, clock, base.key_store.max_keys);
    }
    // Several threads share one limiter: lock-striped, and each stripe gets its
    // share of max_keys so the total stays (about) the configured bound.
    const std::size_t per_shard = (base.key_store.max_keys + kShardCount - 1) / kShardCount;
    return std::make_unique<rl::limit::ShardedLimiter>(
        kShardCount, [&] { return make_local_limiter(base, lc, clock, per_shard); });
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::optional<std::string> listen_override, redis_override, mode_override, threads_override;

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
        } else if (a == "--threads") {
            threads_override = need_arg();
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
            cfg.mode = rl::core::Config::parse_mode(*mode_override);
        }
        if (listen_override) {
            apply_hostport(*listen_override, cfg.server.host, cfg.server.port);
        }
        if (redis_override) {
            apply_hostport(*redis_override, cfg.redis.host, cfg.redis.port);
        }
        if (threads_override) {
            cfg.server.threads = parse_int(*threads_override, "--threads");
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

    std::signal(SIGPIPE, SIG_IGN);

    // Block SIGINT/SIGTERM before any thread exists: new threads inherit the
    // mask, so no worker is ever interrupted and the signal stays pending until
    // the main thread collects it with sigwait() below (design 0.20).
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);

    // Build every Server on this thread so a startup failure (bind, redis
    // config) is reported before anything runs.
    std::vector<std::unique_ptr<rl::http::Server>> servers;
    try {
        std::shared_ptr<rl::limit::RateLimiter> shared;
        if (cfg.mode == rl::core::Config::Mode::local) {
            shared = limiter_factory(cfg.limiter);
        }
        for (int i = 0; i < cfg.server.threads; ++i) {
            std::shared_ptr<rl::limit::RateLimiter> limiter =
                shared ? shared
                       : std::shared_ptr<rl::limit::RateLimiter>(limiter_factory(cfg.limiter));
            servers.push_back(std::make_unique<rl::http::Server>(cfg, std::move(limiter),
                                                                 limiter_factory, stats,
                                                                 /*run_sweeper=*/i == 0));
        }
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }

    const char* mode = (cfg.mode == rl::core::Config::Mode::redis) ? "redis" : "local";
    std::cerr << "ratelimiter listening on " << cfg.server.host << ":" << cfg.server.port
              << " mode=" << mode << " threads=" << cfg.server.threads << "\n";

    // A worker whose loop dies sends SIGTERM to the process, so main wakes up
    // and shuts everything down instead of serving on fewer threads.
    std::atomic<int> exit_code{0};
    std::vector<std::thread> workers;
    try {
        for (auto& server : servers) {
            workers.emplace_back([&exit_code, srv = server.get()] {
                try {
                    srv->run();
                } catch (const std::exception& e) {
                    std::cerr << "fatal: worker: " << e.what() << "\n";
                    exit_code.store(1);
                    ::kill(::getpid(), SIGTERM);
                }
            });
        }
    } catch (const std::exception& e) { // std::thread could not be created
        std::cerr << "fatal: " << e.what() << "\n";
        exit_code.store(1);
        ::kill(::getpid(), SIGTERM);
    }

    int sig = 0;
    sigwait(&stop_signals, &sig);
    for (auto& server : servers) {
        server->stop(); // atomic flag + eventfd write: wakes that thread's epoll_wait
    }
    for (auto& w : workers) {
        w.join();
    }

    std::cerr << "ratelimiter stopped\n";
    return exit_code.load();
}
