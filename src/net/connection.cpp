#include "net/connection.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace rl::net {

namespace {
constexpr std::size_t kReadChunk = 16 * 1024;
}

Connection::Connection(EventLoop& loop,
                       Fd fd,
                       std::string peer_ip,
                       const core::ServerConfig& cfg,
                       RequestHandler& handler,
                       CloseHandler on_close)
    : loop_(loop), fd_(std::move(fd)), fd_num_(fd_.get()), peer_ip_(std::move(peer_ip)), cfg_(cfg),
      handler_(handler), on_close_(std::move(on_close)),
      parser_(cfg.max_header_bytes, cfg.max_body_bytes), last_activity_(core::Clock_t::now()) {
    loop_.add(fd_num_, EPOLLIN, [this](std::uint32_t ev) { on_event(ev); });
}

Connection::~Connection() = default;

void Connection::close() {
    if (dead_) {
        return;
    }
    dead_ = true;
    loop_.del(fd_num_);
    loop_.defer([cb = on_close_, fd = fd_num_] { cb(fd); });
}

void Connection::on_event(std::uint32_t events) {
    if (events & (EPOLLHUP | EPOLLERR)) {
        close();
        return;
    }
    if (events & EPOLLOUT) {
        flush();
        if (dead_) {
            return;
        }
    }
    if (events & EPOLLIN) {
        on_readable();
    }
}

void Connection::on_readable() {
    for (;;) {
        char* dst = in_.reserve(kReadChunk);
        ssize_t n = ::recv(fd_num_, dst, kReadChunk, 0);
        if (n > 0) {
            in_.commit(static_cast<std::size_t>(n));
            if (static_cast<std::size_t>(n) < kReadChunk) {
                break; // likely drained; parse what we have
            }
            continue;
        }
        if (n == 0) { // peer closed
            close();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        close(); // ECONNRESET etc.
        return;
    }

    last_activity_ = core::Clock_t::now();
    process_requests();
    if (!dead_) {
        flush();
    }
}

void Connection::process_requests() {
    for (;;) {
        auto st = parser_.parse(in_);
        if (st == http::RequestParser::Status::need_more) {
            return;
        }

        http::Response resp;
        if (st == http::RequestParser::Status::error) {
            resp.set_status(parser_.error_status());
            resp.keep_alive = false;
            resp.set_json({{"error", http::Response::reason_phrase(parser_.error_status())}});
            closing_ = true;
        } else { // complete
            http::Request req = parser_.request();
            req.peer_ip = peer_ip_;
            resp.keep_alive = req.keep_alive;
            handler_(req, resp);
            if (!resp.keep_alive) {
                closing_ = true;
            }
        }

        out_.append(resp.serialize());
        parser_.reset();

        if (closing_) {
            return; // stop reading further pipelined requests
        }
    }
}

void Connection::flush() {
    while (!out_.empty()) {
        std::string_view v = out_.view();
        ssize_t n = ::send(fd_num_, v.data(), v.size(), MSG_NOSIGNAL);
        if (n > 0) {
            out_.consume(static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        close();
        return;
    }

    if (out_.empty() && closing_) {
        close();
        return;
    }
    update_interest();
}

void Connection::update_interest() {
    const bool want_write = !out_.empty();
    if (want_write == want_write_) {
        return;
    }
    want_write_ = want_write;
    loop_.mod(fd_num_, EPOLLIN | (want_write_ ? EPOLLOUT : 0u));
}

} // namespace rl::net
