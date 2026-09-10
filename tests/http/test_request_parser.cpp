#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "http/request_parser.hpp"
#include "net/buffer.hpp"

using rl::http::RequestParser;
using rl::net::Buffer;
using Status = rl::http::RequestParser::Status;

namespace {
RequestParser make_parser() {
    return RequestParser(/*max_header_bytes=*/1024, /*max_body=*/4096);
}
} // namespace

TEST_CASE("request parser: simple GET in one chunk") {
    auto p = make_parser();
    Buffer buf;
    buf.append("GET /check?key=abc HTTP/1.1\r\nHost: x\r\nUser-Agent: t\r\n\r\n");

    REQUIRE(p.parse(buf) == Status::complete);
    const auto& r = p.request();
    CHECK(r.method == "GET");
    CHECK(r.target == "/check?key=abc");
    CHECK(r.path == "/check");
    CHECK(r.query == "key=abc");
    CHECK(r.http_minor == 1);
    CHECK(r.keep_alive);
    CHECK(r.header("host") != nullptr);
    CHECK(*r.header("HOST") == "x");
    CHECK(buf.empty());
}

TEST_CASE("request parser: byte-at-a-time feeding is resumable") {
    auto p = make_parser();
    const std::string req = "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n";
    Buffer buf;

    for (std::size_t i = 0; i + 1 < req.size(); ++i) {
        buf.append(std::string_view(&req[i], 1));
        REQUIRE(p.parse(buf) == Status::need_more);
    }
    buf.append(std::string_view(&req[req.size() - 1], 1));
    CHECK(p.parse(buf) == Status::complete);
    CHECK(p.request().path == "/healthz");
}

TEST_CASE("request parser: keep-alive rules") {
    SECTION("HTTP/1.1 defaults to keep-alive") {
        auto p = make_parser();
        Buffer b;
        b.append("GET / HTTP/1.1\r\n\r\n");
        REQUIRE(p.parse(b) == Status::complete);
        CHECK(p.request().keep_alive);
    }
    SECTION("HTTP/1.1 with Connection: close") {
        auto p = make_parser();
        Buffer b;
        b.append("GET / HTTP/1.1\r\nConnection: close\r\n\r\n");
        REQUIRE(p.parse(b) == Status::complete);
        CHECK_FALSE(p.request().keep_alive);
    }
    SECTION("HTTP/1.0 defaults to close") {
        auto p = make_parser();
        Buffer b;
        b.append("GET / HTTP/1.0\r\n\r\n");
        REQUIRE(p.parse(b) == Status::complete);
        CHECK_FALSE(p.request().keep_alive);
    }
    SECTION("HTTP/1.0 with Connection: keep-alive") {
        auto p = make_parser();
        Buffer b;
        b.append("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        REQUIRE(p.parse(b) == Status::complete);
        CHECK(p.request().keep_alive);
    }
}

TEST_CASE("request parser: pipelined requests via reset()") {
    auto p = make_parser();
    Buffer b;
    b.append("GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n");

    REQUIRE(p.parse(b) == Status::complete);
    CHECK(p.request().path == "/a");
    p.reset();
    REQUIRE(p.parse(b) == Status::complete);
    CHECK(p.request().path == "/b");
    CHECK(b.empty());
}

TEST_CASE("request parser: Content-Length body is drained, not exposed") {
    auto p = make_parser();
    Buffer b;
    b.append("POST /check HTTP/1.1\r\nContent-Length: 5\r\n\r\nhelloGET /next HTTP/1.1\r\n\r\n");

    REQUIRE(p.parse(b) == Status::complete);
    CHECK(p.request().method == "POST");
    p.reset();
    REQUIRE(p.parse(b) == Status::complete);
    CHECK(p.request().path == "/next");
}

TEST_CASE("request parser: rejects malformed and unsupported requests") {
    SECTION("garbage request line") {
        auto p = make_parser();
        Buffer b;
        b.append("not-a-request\r\n\r\n");
        CHECK(p.parse(b) == Status::error);
        CHECK(p.error_status() == 400);
    }
    SECTION("bad HTTP version") {
        auto p = make_parser();
        Buffer b;
        b.append("GET / HTTP/2.0\r\n\r\n");
        CHECK(p.parse(b) == Status::error);
        CHECK(p.error_status() == 400);
    }
    SECTION("chunked encoding is not implemented") {
        auto p = make_parser();
        Buffer b;
        b.append("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
        CHECK(p.parse(b) == Status::error);
        CHECK(p.error_status() == 501);
    }
    SECTION("oversized headers") {
        auto p = RequestParser(256, 4096);
        Buffer b;
        std::string big = "GET / HTTP/1.1\r\n";
        big += "X-Pad: ";
        big += std::string(400, 'a');
        big += "\r\n\r\n";
        b.append(big);
        CHECK(p.parse(b) == Status::error);
        CHECK(p.error_status() == 431);
    }
    SECTION("Content-Length over the cap") {
        auto p = RequestParser(1024, 8);
        Buffer b;
        b.append("POST / HTTP/1.1\r\nContent-Length: 9\r\n\r\n");
        CHECK(p.parse(b) == Status::error);
        CHECK(p.error_status() == 413);
    }
}
