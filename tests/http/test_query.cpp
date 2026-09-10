#include <catch2/catch_test_macros.hpp>

#include "http/query.hpp"

using rl::http::query_param;
using rl::http::url_decode;

TEST_CASE("url_decode handles percent escapes and plus") {
    CHECK(url_decode("plain") == "plain");
    CHECK(url_decode("a+b") == "a b");
    CHECK(url_decode("a%20b") == "a b");
    CHECK(url_decode("%2Fpath%2F") == "/path/");
    CHECK(url_decode("bad%2") == "bad%2"); // truncated escape kept verbatim
    CHECK(url_decode("%zz") == "%zz");     // non-hex escape kept verbatim
}

TEST_CASE("query_param finds the first matching key") {
    CHECK(query_param("algo=token_bucket&rate=1.5", "algo") == "token_bucket");
    CHECK(query_param("algo=token_bucket&rate=1.5", "rate") == "1.5");
    CHECK(query_param("a=1&a=2", "a") == "1");
}

TEST_CASE("query_param distinguishes absent from empty") {
    CHECK(query_param("a=1&b=2", "c") == std::nullopt);
    CHECK(query_param("a=&b=2", "a") == "");
    CHECK(query_param("flag&b=2", "flag") == ""); // bare key, no '='
}

TEST_CASE("query_param percent-decodes the value") {
    CHECK(query_param("key=a%2Fb", "key") == "a/b");
    CHECK(query_param("key=hello+world", "key") == "hello world");
}

TEST_CASE("query_param on an empty query string") {
    CHECK(query_param("", "anything") == std::nullopt);
}
