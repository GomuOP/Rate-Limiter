#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include "core/clock.hpp"
#include "limit/key_store.hpp"

using namespace std::chrono_literals;
using rl::core::ManualClock;
using rl::limit::KeyStore;

TEST_CASE("key store: get_or_create constructs once, then returns the same value") {
    ManualClock clk;
    KeyStore<int> store(8, 1min, clk);
    int calls = 0;

    int& a = store.get_or_create("a", [&] {
        ++calls;
        return 42;
    });
    CHECK(a == 42);
    CHECK(calls == 1);

    a = 7;
    int& again = store.get_or_create("a", [&] {
        ++calls;
        return 42;
    });
    CHECK(&again == &a);
    CHECK(again == 7);
    CHECK(calls == 1); // factory not called on hit
    CHECK(store.size() == 1);
}

TEST_CASE("key store: evicts the least-recently-used entry past capacity") {
    ManualClock clk;
    KeyStore<int> store(2, 1min, clk);

    store.get_or_create("x", [] { return 1; });
    store.get_or_create("y", [] { return 2; });
    store.get_or_create("x", [] { return -1; }); // touch x -> x is now MRU

    store.get_or_create("z", [] { return 3; }); // capacity 2 -> evict LRU (y)

    CHECK(store.size() == 2);
    CHECK(store.evictions() == 1);

    bool x_recreated = false;
    store.get_or_create("x", [&] {
        x_recreated = true;
        return 0;
    });
    CHECK_FALSE(x_recreated); // x survived

    bool y_recreated = false;
    store.get_or_create("y", [&] {
        y_recreated = true;
        return 0;
    });
    CHECK(y_recreated); // y was evicted
}

TEST_CASE("key store: sweep_expired drops entries idle past the TTL") {
    ManualClock clk;
    KeyStore<int> store(16, 100ms, clk);

    store.get_or_create("a", [] { return 1; });
    store.get_or_create("b", [] { return 2; });

    clk.advance(60ms);
    store.get_or_create("a", [] { return -1; }); // refresh a's last-touch

    clk.advance(60ms); // a: idle 60ms, b: idle 120ms
    CHECK(store.sweep_expired() == 1);
    CHECK(store.size() == 1);

    bool a_recreated = false;
    store.get_or_create("a", [&] {
        a_recreated = true;
        return 0;
    });
    CHECK_FALSE(a_recreated);
}

TEST_CASE("key store: sweep stops at the first live entry") {
    ManualClock clk;
    KeyStore<int> store(16, 100ms, clk);

    store.get_or_create("old1", [] { return 1; });
    store.get_or_create("old2", [] { return 2; });
    clk.advance(200ms);
    store.get_or_create("fresh", [] { return 3; }); // most-recently used

    CHECK(store.sweep_expired() == 2);
    CHECK(store.size() == 1);
}
