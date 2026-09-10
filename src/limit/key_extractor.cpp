#include "limit/key_extractor.hpp"

#include <cctype>
#include <optional>

namespace rl::limit {

namespace {

int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

/// First value for `name` in a raw `a=1&b=2` query string, percent-decoded.
std::optional<std::string> query_value(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        auto amp = query.find('&');
        std::string_view pair = query.substr(0, amp);
        query = (amp == std::string_view::npos) ? std::string_view{} : query.substr(amp + 1);

        auto eq = pair.find('=');
        std::string_view k = pair.substr(0, eq);
        if (k == name) {
            std::string_view v =
                (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
            return KeyExtractor::url_decode(v);
        }
    }
    return std::nullopt;
}

} // namespace

std::string KeyExtractor::url_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < in.size()) {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
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
        if (auto v = query_value(req.query, cfg_.param); v && !v->empty()) {
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
