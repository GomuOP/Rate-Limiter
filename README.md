# rate-limiter

A from-scratch rate limiting service in C++20: a reusable core library, a
hand-written HTTP/1.1 front end, and Redis-backed distributed limiting for
multi-instance deployments. No frameworks in the core — the epoll event loop
and the HTTP parser are written by hand. Third-party libraries are limited to
`nlohmann/json`, `Catch2`, and `hiredis`.

Portfolio project: every design decision, with the alternatives that were
rejected and why, is recorded in [`docs/design.md`](docs/design.md).

---

## What it does

`GET /check?key=<k>` returns `200` if `<k>` is under its limit, `429` with a
`Retry-After` header if not. The limit is enforced by one of:

| algorithm | where the state lives | use |
|---|---|---|
| **token bucket** | in-process, per key | single node, smooth burst allowance |
| **sliding window counter** | in-process, per key | single node, no boundary burst |
| **token bucket in Redis (Lua)** | Redis hash, per key | many nodes sharing one limit |

---

## Architecture

```
                         ┌──────────────────────────────────────────┐
   client ── TCP ───────▶│  Listener (accept4 loop)                  │
                         │      │                                    │
                         │      ▼                                    │
                         │  Connection  ── in Buffer ──▶ RequestParser (resumable
                         │      ▲                         state machine, no chunked)
                         │      │  out Buffer                         │
                         │      │                                    │
                         │   Server::handle ── route /check ─────────┼──▶ KeyExtractor
                         │                          │                │       (query / header / ip)
                         │                          ▼                │
                         │                    RateLimiter::check ────┼──▶ TokenBucket / SlidingWindow
                         │                          │                │       └─ KeyStore (bounded LRU + idle sweep)
                         │                          │                │   or
                         │                          └────────────────┼──▶ RedisRateLimiter
                         │                                           │       └─ hiredis ─ EVALSHA token_bucket.lua
                         │  EventLoop: one epoll fd, one thread,      │
                         │  level-triggered, min-heap timers,         │
                         │  deferred-close queue                      │
                         └──────────────────────────────────────────┘
```

Single-threaded reactor by design (see `docs/design.md` §0.1): the service is
I/O-bound and each check is microseconds of work, so a thread pool would mostly
add lock contention around the key map. Interfaces are kept multi-reactor
friendly — injected clock, limiter behind an abstract interface, per-connection
state owned by the connection object.

### Source layout

```
src/core/    clock (injectable), config (JSON + CLI), stats counters, error category
src/limit/   RateLimiter interface, KeyStore (bounded LRU), TokenBucket, SlidingWindow, KeyExtractor
src/net/     Fd RAII, Buffer, EventLoop (epoll), Listener, Connection
src/http/    Request, RequestParser, Response, Server (routing)
src/redis/   RedisClient (hiredis + reconnect/backoff), RedisRateLimiter, token_bucket.lua
tests/       Catch2 unit tests, mirrors src/
bench/       wrk scripts
```

---

## Build

Requires GCC 13+, CMake 3.24+, Ninja. Dependencies are fetched and pinned by
CMake (`FetchContent`), or picked up from the system if a compatible version is
installed.

```sh
cmake --preset debug      # Debug, ASan + UBSan
cmake --build --preset debug
ctest --preset debug      # 33 unit tests

cmake --preset release    # -O3, LTO
cmake --build --preset release
```

## Run

```sh
./build/release/ratelimiter --config config/ratelimiter.example.json
```

CLI overrides: `--config <path>`, `--listen <host:port>`, `--mode local|redis`,
`--redis <host:port>`.

```sh
# local token bucket, 5 rps burst 10, key from ?key=
./build/release/ratelimiter --listen 127.0.0.1:8080

curl -i 'http://127.0.0.1:8080/check?key=user-42'
#   HTTP/1.1 200 OK
#   X-RateLimit-Limit: 10
#   X-RateLimit-Remaining: 9
#   {"allowed":true,"limit":10,"remaining":9,"reset_ms":200}

# ... once the bucket is empty:
#   HTTP/1.1 429 Too Many Requests
#   Retry-After: 1
#   {"allowed":false,"limit":10,"remaining":0,"reset_ms":2000,"retry_after_ms":200}
```

### Distributed mode

```sh
redis-server --port 6379 &
./build/release/ratelimiter --mode redis --redis 127.0.0.1:6379 --listen 127.0.0.1:8081 &
./build/release/ratelimiter --mode redis --redis 127.0.0.1:6379 --listen 127.0.0.1:8082 &
# both instances now enforce one shared bucket per key
```

---

## HTTP API

| route | method | response |
|---|---|---|
| `/` , `/dashboard` | GET | a self-contained HTML status page (see below) |
| `/check` | GET, HEAD | `200` / `429` + JSON `{allowed, limit, remaining, reset_ms[, retry_after_ms]}`; headers `X-RateLimit-Limit`, `X-RateLimit-Remaining`, and `Retry-After` (seconds) on `429` |
| `/healthz` | GET | `200 ok` |
| `/stats` | GET | JSON counters: requests, allowed, denied, bad_requests, config_changes, conns, key_count, evictions, sweeps_removed, redis_errors, redis_failopen/failclosed, uptime_ms |
| `/config` | GET | JSON of the limiter config in effect: `{algo, rate, burst, limit, window_ms}` |
| `/config` | POST | change the limiter live (local mode only). Params in the **query string**: `algo`, `rate`, `burst`, `limit`, `window_ms` — any subset; omitted fields keep their current value. `200` + the new config on success, `400` on a bad/out-of-range value, `409` in `redis` mode |

### Status page

Open `http://<host>:<port>/` in a browser. It is served straight from the
`ratelimiter` process (no framework, no external requests), polls `/stats` once
a second, shows the live counters and a derived requests/sec, has a small
form to fire `/check?key=` calls so you can watch `200` turn into `429`, and a
panel that switches the algorithm and its parameters live via `POST /config`.

`POST /config` rebuilds the limiter from an empty key store, so existing
buckets/windows are cleared; the `/stats` counters keep running. Example:

```
curl -X POST 'http://127.0.0.1:8080/config?algo=sliding_window&limit=10&window_ms=10000'
curl -X POST 'http://127.0.0.1:8080/config?algo=token_bucket&rate=1&burst=5'
```

Key source is configurable (`key_extractor.source`): `query` (`?key=`),
`header` (a named header), or `ip` (peer address, or the left-most
`X-Forwarded-For` entry when `trust_forwarded_for` is set).

---

## Configuration

See [`config/ratelimiter.example.json`](config/ratelimiter.example.json).
Blocks: `server`, `limiter` (`algo`, `rate`, `burst`, `limit`, `window_ms`),
`key_store` (`max_keys`, `idle_ttl_ms`, `sweep_interval_ms`), `key_extractor`,
`redis` (`timeout_ms`, `fail_closed`, `connect_backoff_ms`).

---

## Design decisions (summary)

Full log with rejected alternatives in [`docs/design.md`](docs/design.md).

- **Single-threaded reactor.** I/O-bound workload; avoids locking the key map.
- **Lazy refill / lazy decay.** State carries a timestamp; elapsed time is
  folded in on each `check()`. No timer per key — O(1) per request, idle keys
  cost nothing.
- **Sliding-window counter, not a log.** A per-key timestamp log is exact but
  its memory grows with the limit and with traffic — a DoS vector. The
  two-window weighted counter removes the fixed-window 2× boundary burst for
  one multiply, with a bounded approximation error (below).
- **Bounded key store.** `unordered_map` + intrusive LRU; evict LRU past
  `max_keys`, plus an idle-TTL sweep from the loop timer. An unbounded map
  lets an attacker vary the key and drive the process OOM.
- **Redis atomicity via one Lua script.** `GET` then `SET` from the client is a
  read-modify-write race between instances; the whole bucket update runs as one
  `EVALSHA` (with `SCRIPT LOAD`/`EVAL` fallback on `NOSCRIPT`). The script is
  deterministic — time is passed in as an argument, not read via
  `redis.call('TIME')` — so it is replication- and AOF-safe.
- **Fail open by default.** If Redis is unreachable the limiter allows the
  request and bumps `redis_failopen`; `redis.fail_closed` flips this. For a
  limiter, availability of the protected service usually outweighs a brief
  window of over-admission.
- **Errors are values on the hot path.** `std::error_code` with a project
  `error_category`; exceptions only during startup.

### Sliding-window approximation error

The estimate assumes requests in the previous window were uniformly
distributed. If the previous window's hits were actually clustered at its end,
the estimate under-counts and the limiter admits slightly more than the true
rolling count; if clustered at the start, it over-counts and admits slightly
fewer. The error is bounded by `prev_count` and shrinks linearly to zero across
the current window. For steady traffic it is negligible; token bucket is the
better choice when exactness at the edge matters.

---

## Benchmarks

Method: release build (`-O3`, LTO), single-threaded server pinned to loopback on
one machine (6.8 kernel, GCC 13). Load from `wrk` on the same host, so numbers
are dominated by loopback syscall cost and the client competing for cores —
treat them as a lower bound and a regression baseline, not a datasheet figure.
Reproduce with `bench/run.sh`.

| profile | `wrk` | throughput | p50 | p90 | p99 |
|---|---|---:|---:|---:|---:|
| `/healthz`, keep-alive | `-t4 -c128 -d10s` | **72,100 req/s** | 1.86 ms | 2.05 ms | 3.78 ms |
| `/check`, 2000 keys, generous limit | `-t4 -c128 -d10s` | **97,900 req/s** | 1.20 ms | 1.50 ms | 2.58 ms |
| `/healthz`, `Connection: close` (1 req/conn) | `-t4 -c128 -d10s` | **19,100 req/s** | 4.14 ms | 7.16 ms | 14.06 ms |
| `/check`, 10k connections, ASan build | `-t8 -c10000 -d20s` | 10,900 req/s | — | — | — |

The 10k-connection run is on the **debug + ASan/UBSan** build and is a
correctness check, not a speed one: 10,002 connections accepted, clean
`SIGINT` shutdown, no leak or UB reports.

Connection churn is ~4× slower than keep-alive because each request pays a full
`accept`/`close` and a 3-way handshake; with keep-alive the accept cost
amortises to zero.

---

## Known limitations

- **Synchronous Redis on the event loop.** In `redis` mode each `/check` makes
  one blocking `hiredis` call; a slow Redis stalls the loop for up to
  `redis.timeout_ms`. A dropped connection is retried only once per
  `connect_backoff_ms`, so a *down* Redis costs one timeout per backoff window,
  not one per request. The real fix — the hiredis async adapter multiplexed on
  the same epoll loop, turning `/check` into a continuation — is the documented
  next step (`docs/design.md` §0.6).
- **Single core.** One reactor thread. The interfaces are multi-reactor ready
  (`SO_REUSEPORT`, one loop per core) but that work is deferred until a load
  test shows a single core is the bottleneck.
- **Redis mode uses wall-clock time** (`system_clock`) so instances agree on
  refill; it assumes their clocks are within a small skew (NTP).
- **No TLS, HTTP/1.1 only, no chunked request bodies.** Terminate TLS at a
  front proxy. Request bodies are drained and ignored (`/check` is a GET).
- **One Redis node.** No Cluster or Sentinel failover.
