#include "http/query.hpp"

namespace rl::http {

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

} // namespace

std::string url_decode(std::string_view in) {
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

std::optional<std::string> query_param(std::string_view query, std::string_view name) {
    while (!query.empty()) {
        auto amp = query.find('&');
        std::string_view pair = query.substr(0, amp);
        query = (amp == std::string_view::npos) ? std::string_view{} : query.substr(amp + 1);

        auto eq = pair.find('=');
        std::string_view k = pair.substr(0, eq);
        if (k == name) {
            std::string_view v =
                (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
            return url_decode(v);
        }
    }
    return std::nullopt;
}

} // namespace rl::http
