#include <catch2/catch_test_macros.hpp>

#include "core/config.hpp"
#include "http/request.hpp"
#include "limit/key_extractor.hpp"

using rl::core::KeyExtractorConfig;
using rl::http::Request;
using rl::limit::KeyExtractor;

namespace {

Request make_request(std::string query = {},
                     std::string peer_ip = "203.0.113.7",
                     std::vector<std::pair<std::string, std::string>> headers = {}) {
    Request r;
    r.method = "GET";
    r.path = "/check";
    r.query = std::move(query);
    r.peer_ip = std::move(peer_ip);
    r.headers = std::move(headers);
    return r;
}

KeyExtractorConfig cfg(KeyExtractorConfig::Source src,
                       std::string param,
                       bool fallback_ip = true,
                       bool trust_xff = false) {
    KeyExtractorConfig c;
    c.source = src;
    c.param = std::move(param);
    c.fallback_ip = fallback_ip;
    c.trust_forwarded_for = trust_xff;
    return c;
}

} // namespace

TEST_CASE("key extractor: url_decode handles percent escapes and plus") {
    CHECK(KeyExtractor::url_decode("plain") == "plain");
    CHECK(KeyExtractor::url_decode("a+b") == "a b");
    CHECK(KeyExtractor::url_decode("a%20b") == "a b");
    CHECK(KeyExtractor::url_decode("%2Fpath%2F") == "/path/");
    CHECK(KeyExtractor::url_decode("bad%2") == "bad%2"); // truncated escape kept verbatim
}

TEST_CASE("key extractor: query source") {
    KeyExtractor ex(cfg(KeyExtractorConfig::Source::query, "key"));

    auto r = ex.extract(make_request("key=user-42&x=1"));
    CHECK(r.ok);
    CHECK(r.key == "user-42");

    auto decoded = ex.extract(make_request("key=a%20b"));
    CHECK(decoded.ok);
    CHECK(decoded.key == "a b");
}

TEST_CASE("key extractor: missing query key falls back to peer IP when allowed") {
    KeyExtractor ex(cfg(KeyExtractorConfig::Source::query, "key", /*fallback_ip=*/true));
    auto r = ex.extract(make_request("other=1", "198.51.100.4"));
    CHECK(r.ok);
    CHECK(r.key == "198.51.100.4");
}

TEST_CASE("key extractor: missing query key is a 400 when fallback is off") {
    KeyExtractor ex(cfg(KeyExtractorConfig::Source::query, "key", /*fallback_ip=*/false));
    auto r = ex.extract(make_request("other=1"));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("key") != std::string::npos);
}

TEST_CASE("key extractor: header source is case-insensitive") {
    KeyExtractor ex(cfg(KeyExtractorConfig::Source::header, "X-Api-Key", /*fallback_ip=*/false));
    auto r = ex.extract(make_request({}, "203.0.113.7", {{"x-api-key", "abc"}}));
    CHECK(r.ok);
    CHECK(r.key == "abc");

    auto missing = ex.extract(make_request());
    CHECK_FALSE(missing.ok);
}

TEST_CASE("key extractor: ip source") {
    KeyExtractor ex(cfg(KeyExtractorConfig::Source::ip, "key"));
    auto r = ex.extract(make_request({}, "192.0.2.55"));
    CHECK(r.ok);
    CHECK(r.key == "192.0.2.55");
}

TEST_CASE("key extractor: X-Forwarded-For is only honoured when trusted") {
    auto req = make_request({}, "10.0.0.1", {{"X-Forwarded-For", "9.9.9.9, 10.0.0.1"}});

    KeyExtractor untrusting(cfg(KeyExtractorConfig::Source::ip, "key", true, /*trust_xff=*/false));
    CHECK(untrusting.extract(req).key == "10.0.0.1");

    KeyExtractor trusting(cfg(KeyExtractorConfig::Source::ip, "key", true, /*trust_xff=*/true));
    CHECK(trusting.extract(req).key == "9.9.9.9");
}
