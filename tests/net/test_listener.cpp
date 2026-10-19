#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <system_error>

#include "net/event_loop.hpp"
#include "net/listener.hpp"

using rl::net::EventLoop;
using rl::net::Listener;

namespace {

std::uint16_t bound_port(const Listener& l) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    ::getsockname(l.fd(), reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

const Listener::AcceptHandler kIgnore = [](rl::net::Fd, std::string) {};

} // namespace

TEST_CASE("listener: SO_REUSEPORT lets two listeners share a port") {
    EventLoop loop;
    Listener first(loop, "127.0.0.1", 0, 16, /*reuse_port=*/true, kIgnore);
    const std::uint16_t port = bound_port(first);
    REQUIRE(port != 0);

    Listener second(loop, "127.0.0.1", port, 16, /*reuse_port=*/true, kIgnore);
    CHECK(bound_port(second) == port);
}

TEST_CASE("listener: without SO_REUSEPORT a second bind fails") {
    EventLoop loop;
    Listener first(loop, "127.0.0.1", 0, 16, /*reuse_port=*/false, kIgnore);
    const std::uint16_t port = bound_port(first);

    // SO_REUSEADDR alone does not allow two sockets listening on one port.
    CHECK_THROWS_AS(Listener(loop, "127.0.0.1", port, 16, /*reuse_port=*/false, kIgnore),
                    std::system_error);
}
