#pragma once

#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace rl::http {

/// Response builder. The connection layer calls `serialize()` and writes the
/// result; `Content-Length` and `Connection` are added at serialize time.
struct Response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool keep_alive = true;

    void set_status(int code) { status = code; }
    void add_header(std::string k, std::string v) {
        headers.emplace_back(std::move(k), std::move(v));
    }
    void set_text(std::string s) {
        add_header("Content-Type", "text/plain; charset=utf-8");
        body = std::move(s);
    }
    void set_json(const nlohmann::json& j) {
        add_header("Content-Type", "application/json");
        body = j.dump();
    }
    void set_html(std::string s) {
        add_header("Content-Type", "text/html; charset=utf-8");
        body = std::move(s);
    }

    static const char* reason_phrase(int code);
    std::string serialize() const;
};

} // namespace rl::http
