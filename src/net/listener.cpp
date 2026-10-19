#include "net/listener.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace rl::net {

namespace {

Fd make_listening_socket(const std::string& host,
                         std::uint16_t port,
                         int backlog,
                         bool reuse_port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    const std::string port_str = std::to_string(port);
    addrinfo* res = nullptr;
    if (int gai = ::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res); gai != 0) {
        throw std::runtime_error("getaddrinfo(" + host + "): " + ::gai_strerror(gai));
    }

    Fd sock(::socket(res->ai_family, res->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     res->ai_protocol));
    if (!sock) {
        int e = errno;
        ::freeaddrinfo(res);
        throw std::system_error(e, std::system_category(), "socket");
    }

    int one = 1;
    ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // One listening socket per worker thread, all on the same port; the kernel
    // hashes each new connection to one of them (design 0.20). Off for a single
    // thread so a second instance on the same port still fails with EADDRINUSE.
    if (reuse_port && ::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
        int e = errno;
        ::freeaddrinfo(res);
        throw std::system_error(e, std::system_category(), "setsockopt SO_REUSEPORT");
    }

    if (::bind(sock.get(), res->ai_addr, res->ai_addrlen) != 0) {
        int e = errno;
        ::freeaddrinfo(res);
        throw std::system_error(e, std::system_category(), "bind " + host + ":" + port_str);
    }
    ::freeaddrinfo(res);

    if (::listen(sock.get(), backlog) != 0) {
        throw std::system_error(errno, std::system_category(), "listen");
    }
    return sock;
}

} // namespace

Listener::Listener(EventLoop& loop,
                   const std::string& host,
                   std::uint16_t port,
                   int backlog,
                   bool reuse_port,
                   AcceptHandler on_accept)
    : loop_(loop), sock_(make_listening_socket(host, port, backlog, reuse_port)),
      on_accept_(std::move(on_accept)) {
    loop_.add(sock_.get(), EPOLLIN, [this](std::uint32_t) { on_readable(); });
}

Listener::~Listener() {
    if (sock_) {
        loop_.del(sock_.get());
    }
}

void Listener::on_readable() {
    for (;;) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int cfd = ::accept4(sock_.get(), reinterpret_cast<sockaddr*>(&addr), &len,
                            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; // drained
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                return; // out of fds; try again on the next readiness event
            }
            return;
        }

        char ipbuf[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
        on_accept_(Fd(cfd), std::string(ipbuf));
    }
}

} // namespace rl::net
