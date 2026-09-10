#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rl::http {

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

struct Request {
    std::string method;
    std::string target; // raw request target, includes any query
    std::string path;   // decoded path portion (no query)
    std::string query;  // raw query string (portion after '?', may be empty)
    int http_minor = 1; // HTTP/1.<minor>
    std::vector<std::pair<std::string, std::string>> headers;
    bool keep_alive = true;
    std::string peer_ip; // set by the connection layer

    /// First header value matching `name` (case-insensitive), or nullptr.
    const std::string* header(std::string_view name) const {
        for (const auto& [k, v] : headers) {
            if (iequals(k, name)) {
                return &v;
            }
        }
        return nullptr;
    }
};

} // namespace rl::http
