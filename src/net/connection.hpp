#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/clock.hpp"
#include "core/config.hpp"
#include "http/request.hpp"
#include "http/request_parser.hpp"
#include "http/response.hpp"
#include "net/buffer.hpp"
#include "net/event_loop.hpp"
#include "net/fd.hpp"

namespace rl::net {

/// One client connection. Reads into an input buffer, feeds the parser, and for
/// each completed request calls the handler and queues the serialized response.
/// Supports keep-alive and partial writes (EPOLLOUT). Closing is deferred
/// through the loop so no fd is freed while the current epoll batch still holds
/// events for it.
class Connection {
  public:
    using RequestHandler = std::function<void(const http::Request&, http::Response&)>;
    using CloseHandler = std::function<void(int fd)>;

    Connection(EventLoop& loop,
               Fd fd,
               std::string peer_ip,
               const core::ServerConfig& cfg,
               RequestHandler& handler,
               CloseHandler on_close);
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    core::TimePoint last_activity() const { return last_activity_; }
    void close();

  private:
    void on_event(std::uint32_t events);
    void on_readable();
    void process_requests();
    void flush();
    void update_interest();

    EventLoop& loop_;
    Fd fd_;
    int fd_num_; // stable copy for del()/defer() after fd_ may be cleared
    std::string peer_ip_;
    const core::ServerConfig& cfg_;
    RequestHandler& handler_;
    CloseHandler on_close_;

    Buffer in_;
    Buffer out_;
    http::RequestParser parser_;
    core::TimePoint last_activity_;
    bool want_write_ = false;
    bool closing_ = false; // finish flushing, then close
    bool dead_ = false;
};

} // namespace rl::net
