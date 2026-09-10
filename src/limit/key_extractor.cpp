#include "limit/key_extractor.hpp"

#include <cctype>
#include <optional>

#include "http/query.hpp"

namespace rl::limit {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

std::string KeyExtractor::url_decode(std::string_view in) {
    return http::url_decode(in);
}

KeyExtractor::Result KeyExtractor::extract(const http::Request& req) const {
    using Source = core::KeyExtractorConfig::Source;

    auto peer_ip_key = [&]() -> Result {
        if (cfg_.trust_forwarded_for) {
            if (const std::string* xff = req.header("X-Forwarded-For")) {
                std::string_view v = *xff;
                auto comma = v.find(',');
                std::string_view first = trim(v.substr(0, comma));
                if (!first.empty()) {
                    return {true, std::string(first), {}};
                }
            }
        }
        if (req.peer_ip.empty()) {
            return {false, {}, "no peer address available"};
        }
        return {true, req.peer_ip, {}};
    };

    switch (cfg_.source) {
    case Source::ip:
        return peer_ip_key();

    case Source::header: {
        if (const std::string* h = req.header(cfg_.param); h && !h->empty()) {
            return {true, *h, {}};
        }
        if (cfg_.fallback_ip) {
            return peer_ip_key();
        }
        return {false, {}, "missing required header '" + cfg_.param + "'"};
    }

    case Source::query: {
        if (auto v = http::query_param(req.query, cfg_.param); v && !v->empty()) {
            return {true, std::move(*v), {}};
        }
        if (cfg_.fallback_ip) {
            return peer_ip_key();
        }
        return {false, {}, "missing required query parameter '" + cfg_.param + "'"};
    }
    }
    return {false, {}, "unreachable"};
}

} // namespace rl::limit
