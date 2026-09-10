#pragma once

#include <cstddef>

#include "http/request.hpp"
#include "net/buffer.hpp"

namespace rl::http {

/// Resumable HTTP/1.1 request-line + header parser (design scope: no chunked
/// encoding; bodies are drained, not exposed). Fed incrementally from a Buffer;
/// bytes it accepts are consumed from the buffer, the rest stay for the next
/// call. One instance is reused across keep-alive requests via `reset()`.
class RequestParser {
  public:
    enum class Status { need_more, complete, error };

    RequestParser(std::size_t max_header_bytes, std::size_t max_body_bytes);

    Status parse(net::Buffer& buf);

    const Request& request() const { return req_; }
    int error_status() const { return error_status_; }

    void reset();

  private:
    enum class State { request_line, headers, body, done, failed };

    Status fail(int status);
    bool parse_request_line(std::string_view line);
    bool parse_header_line(std::string_view line);
    void finalize_headers();

    State state_ = State::request_line;
    Request req_;
    std::size_t max_header_bytes_;
    std::size_t max_body_bytes_;
    std::size_t header_bytes_ = 0;
    std::size_t body_remaining_ = 0;
    int error_status_ = 400;
};

} // namespace rl::http
