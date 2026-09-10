#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

struct redisContext;
struct redisReply;

namespace rl::redis {

/// Thin synchronous hiredis wrapper with reconnect-and-backoff (design 0.6).
///
/// The single blocking call per `/check` can stall the event loop for up to
/// `timeout`; that is the documented trade for not building the async adapter
/// in the sprint. On any failure the connection is dropped and reconnection is
/// not retried until `backoff` has elapsed, so a down Redis costs one timeout
/// per `backoff` window, not one per request.
class RedisClient {
  public:
    RedisClient(std::string host,
                std::uint16_t port,
                std::chrono::milliseconds timeout,
                std::chrono::milliseconds backoff);
    ~RedisClient();

    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    bool connected() const;

    /// Connect if not connected and the backoff window has elapsed.
    /// Returns true if a usable connection exists afterwards.
    bool ensure();

    /// Owned reply (free with freeReplyObject) or nullptr on failure. A nullptr
    /// return drops the connection and starts the backoff timer.
    redisReply* command_argv(int argc, const char** argv, const std::size_t* argvlen);

  private:
    void connect_now();
    void drop();

    std::string host_;
    std::uint16_t port_;
    std::chrono::milliseconds timeout_;
    std::chrono::milliseconds backoff_;
    redisContext* ctx_ = nullptr;
    std::chrono::steady_clock::time_point next_attempt_{};
};

} // namespace rl::redis
