#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "net/event_loop.hpp"
#include "net/fd.hpp"

namespace rl::net {

/// Listening TCP socket. Accepts in a loop until EAGAIN and hands each new
/// connection (already non-blocking, CLOEXEC) plus its peer IP to the handler.
/// Construction throws std::system_error on failure (startup path, design 0.10).
class Listener {
  public:
    using AcceptHandler = std::function<void(Fd conn, std::string peer_ip)>;

    Listener(EventLoop& loop,
             const std::string& host,
             std::uint16_t port,
             int backlog,
             AcceptHandler on_accept);
    ~Listener();

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    int fd() const { return sock_.get(); }

  private:
    void on_readable();

    EventLoop& loop_;
    Fd sock_;
    AcceptHandler on_accept_;
};

} // namespace rl::net
