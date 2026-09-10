#include "redis/redis_rate_limiter.hpp"

#include <hiredis.h>

#include <array>
#include <chrono>
#include <cstring>
#include <string>

#include "rl/lua_script.inc" // provides rl::redis::kTokenBucketLua

namespace rl::redis {

namespace {

using Ms = std::chrono::milliseconds;

struct ReplyGuard {
    redisReply* r;
    ~ReplyGuard() {
        if (r != nullptr) {
            ::freeReplyObject(r);
        }
    }
};

std::int64_t wall_now_ms() {
    return std::chrono::duration_cast<Ms>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

RedisRateLimiter::RedisRateLimiter(const core::RedisConfig& rcfg,
                                   double rate,
                                   std::int64_t burst,
                                   core::Stats& stats)
    : client_(rcfg.host, rcfg.port, Ms(rcfg.timeout_ms), Ms(rcfg.connect_backoff_ms)), rate_(rate),
      burst_(burst), fail_closed_(rcfg.fail_closed), stats_(stats) {
}

bool RedisRateLimiter::load_script() {
    const std::size_t script_len = std::strlen(kTokenBucketLua);
    std::array<const char*, 3> argv{"SCRIPT", "LOAD", kTokenBucketLua};
    std::array<std::size_t, 3> lens{6, 4, script_len};

    redisReply* reply = client_.command_argv(3, argv.data(), lens.data());
    if (reply == nullptr) {
        return false;
    }
    ReplyGuard g{reply};
    if (reply->type != REDIS_REPLY_STRING) {
        return false;
    }
    sha_.assign(reply->str, reply->len);
    return true;
}

limit::Decision RedisRateLimiter::fail_decision(std::int64_t /*cost*/) {
    limit::Decision d;
    d.limit = burst_;
    if (fail_closed_) {
        ++stats_.redis_failclosed;
        d.allowed = false;
        d.remaining = 0;
        d.retry_after = Ms(1000);
        d.reset = Ms(1000);
    } else {
        ++stats_.redis_failopen;
        d.allowed = true;
        d.remaining = burst_;
    }
    return d;
}

limit::Decision RedisRateLimiter::check(std::string_view key, std::int64_t cost) {
    if (!client_.ensure()) {
        ++stats_.redis_errors;
        return fail_decision(cost);
    }
    if (sha_.empty() && !load_script()) {
        ++stats_.redis_errors;
        return fail_decision(cost);
    }

    const std::string keystr(key);
    const std::string now_s = std::to_string(wall_now_ms());
    const std::string rate_s = std::to_string(rate_);
    const std::string burst_s = std::to_string(burst_);
    const std::string cost_s = std::to_string(cost);

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::array<const char*, 8> argv{"EVALSHA",       sha_.c_str(),  "1",
                                        keystr.c_str(),  now_s.c_str(), rate_s.c_str(),
                                        burst_s.c_str(), cost_s.c_str()};
        std::array<std::size_t, 8> lens{7,
                                        sha_.size(),
                                        1,
                                        keystr.size(),
                                        now_s.size(),
                                        rate_s.size(),
                                        burst_s.size(),
                                        cost_s.size()};

        redisReply* reply = client_.command_argv(8, argv.data(), lens.data());
        if (reply == nullptr) {
            ++stats_.redis_errors;
            return fail_decision(cost);
        }
        ReplyGuard g{reply};

        if (reply->type == REDIS_REPLY_ERROR) {
            if (attempt == 0 && reply->str != nullptr &&
                std::strncmp(reply->str, "NOSCRIPT", 8) == 0) {
                sha_.clear();
                if (!load_script()) {
                    ++stats_.redis_errors;
                    return fail_decision(cost);
                }
                continue; // retry EVALSHA with the freshly loaded script
            }
            ++stats_.redis_errors;
            return fail_decision(cost);
        }

        if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 4) {
            ++stats_.redis_errors;
            return fail_decision(cost);
        }

        limit::Decision d;
        d.limit = burst_;
        d.allowed = reply->element[0]->integer != 0;
        d.remaining = reply->element[1]->integer;
        d.retry_after = Ms(reply->element[2]->integer);
        d.reset = Ms(reply->element[3]->integer);
        return d;
    }

    ++stats_.redis_errors;
    return fail_decision(cost);
}

} // namespace rl::redis
