#include "http/request_parser.hpp"

#include <cctype>
#include <charconv>

namespace rl::http {

namespace {

constexpr std::string_view kCRLF = "\r\n";

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

bool contains_token(std::string_view value, std::string_view token) {
    while (!value.empty()) {
        auto comma = value.find(',');
        std::string_view part = trim(value.substr(0, comma));
        if (iequals(part, token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return false;
}

bool has_ctl(std::string_view s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

} // namespace

RequestParser::RequestParser(std::size_t max_header_bytes, std::size_t max_body_bytes)
    : max_header_bytes_(max_header_bytes), max_body_bytes_(max_body_bytes) {
}

void RequestParser::reset() {
    state_ = State::request_line;
    req_ = Request{};
    header_bytes_ = 0;
    body_remaining_ = 0;
    error_status_ = 400;
}

RequestParser::Status RequestParser::fail(int status) {
    state_ = State::failed;
    error_status_ = status;
    return Status::error;
}

bool RequestParser::parse_request_line(std::string_view line) {
    if (has_ctl(line)) {
        error_status_ = 400;
        return false;
    }
    auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) {
        return false;
    }
    auto rest = line.substr(sp1 + 1);
    auto sp2 = rest.find(' ');
    if (sp2 == std::string_view::npos) {
        return false;
    }

    std::string_view method = line.substr(0, sp1);
    std::string_view target = rest.substr(0, sp2);
    std::string_view version = rest.substr(sp2 + 1);

    if (method.empty() || method.size() > 16) {
        return false;
    }
    if (version.size() != 8 || version.substr(0, 7) != "HTTP/1." ||
        (version[7] != '0' && version[7] != '1')) {
        return false;
    }
    if (target.empty() || target.front() != '/') {
        return false; // only origin-form is supported
    }

    req_.method.assign(method);
    req_.target.assign(target);
    req_.http_minor = version[7] - '0';

    auto q = target.find('?');
    if (q == std::string_view::npos) {
        req_.path.assign(target);
        req_.query.clear();
    } else {
        req_.path.assign(target.substr(0, q));
        req_.query.assign(target.substr(q + 1));
    }
    return true;
}

bool RequestParser::parse_header_line(std::string_view line) {
    auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    std::string_view name = trim(line.substr(0, colon));
    std::string_view value = trim(line.substr(colon + 1));
    if (name.empty() || has_ctl(name)) {
        return false;
    }
    if (req_.headers.size() >= 100) {
        error_status_ = 431;
        return false;
    }
    req_.headers.emplace_back(std::string(name), std::string(value));
    return true;
}

void RequestParser::finalize_headers() {
    req_.keep_alive = req_.http_minor >= 1;
    if (const std::string* c = req_.header("Connection")) {
        if (contains_token(*c, "close")) {
            req_.keep_alive = false;
        } else if (contains_token(*c, "keep-alive")) {
            req_.keep_alive = true;
        }
    }

    if (const std::string* te = req_.header("Transfer-Encoding")) {
        if (!iequals(*te, "identity")) {
            state_ = State::failed;
            error_status_ = 501; // chunked and friends are out of scope
            return;
        }
    }

    if (const std::string* cl = req_.header("Content-Length")) {
        std::string_view v = *cl;
        std::size_t n = 0;
        auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
        if (ec != std::errc{} || ptr != v.data() + v.size()) {
            state_ = State::failed;
            error_status_ = 400;
            return;
        }
        if (n > max_body_bytes_) {
            state_ = State::failed;
            error_status_ = 413;
            return;
        }
        body_remaining_ = n;
    }

    state_ = (body_remaining_ > 0) ? State::body : State::done;
}

RequestParser::Status RequestParser::parse(net::Buffer& buf) {
    for (;;) {
        switch (state_) {
        case State::request_line:
        case State::headers: {
            std::string_view view = buf.view();
            auto pos = view.find(kCRLF);
            if (pos == std::string_view::npos) {
                if (view.size() > max_header_bytes_) {
                    return fail(431);
                }
                return Status::need_more;
            }
            std::string_view line = view.substr(0, pos);
            header_bytes_ += pos + 2;
            if (header_bytes_ > max_header_bytes_) {
                return fail(431);
            }
            buf.consume(pos + 2);

            if (state_ == State::request_line) {
                if (!parse_request_line(line)) {
                    return fail(error_status_);
                }
                state_ = State::headers;
            } else if (line.empty()) {
                finalize_headers();
                if (state_ == State::failed) {
                    return Status::error;
                }
            } else if (!parse_header_line(line)) {
                return fail(error_status_);
            }
            break;
        }

        case State::body: {
            std::size_t n = std::min(buf.readable(), body_remaining_);
            buf.consume(n);
            body_remaining_ -= n;
            if (body_remaining_ > 0) {
                return Status::need_more;
            }
            state_ = State::done;
            break;
        }

        case State::done:
            return Status::complete;

        case State::failed:
            return Status::error;
        }
    }
}

} // namespace rl::http
