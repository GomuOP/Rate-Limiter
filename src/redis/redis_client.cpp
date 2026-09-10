#include "redis/redis_client.hpp"

#include <hiredis.h>

#include <utility>

namespace rl::redis {

RedisClient::RedisClient(std::string host,
                         std::uint16_t port,
                         std::chrono::milliseconds timeout,
                         std::chrono::milliseconds backoff)
    : host_(std::move(host)), port_(port), timeout_(timeout), backoff_(backoff) {
}

RedisClient::~RedisClient() {
    drop();
}

bool RedisClient::connected() const {
    return ctx_ != nullptr && ctx_->err == 0;
}

void RedisClient::drop() {
    if (ctx_ != nullptr) {
        ::redisFree(ctx_);
        ctx_ = nullptr;
    }
}

void RedisClient::connect_now() {
    drop();
    timeval tv{};
    tv.tv_sec = timeout_.count() / 1000;
    tv.tv_usec = (timeout_.count() % 1000) * 1000;

    ctx_ = ::redisConnectWithTimeout(host_.c_str(), port_, tv);
    if (ctx_ == nullptr || ctx_->err != 0) {
        drop();
        next_attempt_ = std::chrono::steady_clock::now() + backoff_;
        return;
    }
    ::redisSetTimeout(ctx_, tv); // applies to subsequent command reads
}

bool RedisClient::ensure() {
    if (connected()) {
        return true;
    }
    if (std::chrono::steady_clock::now() < next_attempt_) {
        return false; // still in the backoff window
    }
    connect_now();
    return connected();
}

redisReply* RedisClient::command_argv(int argc, const char** argv, const std::size_t* argvlen) {
    if (!connected()) {
        return nullptr;
    }
    auto* reply = static_cast<redisReply*>(::redisCommandArgv(ctx_, argc, argv, argvlen));
    if (reply == nullptr) {
        // protocol / IO error: context is now unusable
        drop();
        next_attempt_ = std::chrono::steady_clock::now() + backoff_;
        return nullptr;
    }
    return reply;
}

} // namespace rl::redis
