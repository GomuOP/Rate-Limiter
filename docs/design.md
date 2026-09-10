# Design log

Every design decision for this project is recorded here, newest phase last.
Each entry states the decision, the alternatives that were rejected, and why.

Format for each entry:

- **Decision** — what we are doing.
- **Rejected** — the alternatives and the reason each was turned down.
- **Revisit if** — the condition under which this should be reopened (optional).

> **History note.** This repo originally planned a full reverse proxy (epoll
> loop, HTTP parser, upstream pool, round-robin LB, active health checks, a
> Next.js dashboard, Redis-distributed limiting on top). On 2026-09-10 the
> scope was cut to a **standalone rate limiting service** and the working mode
> switched to a straight-through build. The proxy-era decisions below are kept
> where they still apply (threading, CI, deps, error handling, warnings,
> config, namespace, formatting, layout, presets) and re-scoped where needed.

---

## Phase 0 — Scaffolding

Status: agreed 2026-09-10.

### 0.1 Threading model

- **Decision** — Single-threaded epoll reactor for the HTTP service. One
  `EventLoop`, one thread. The core library is **not** internally
  thread-safe; safety comes from being driven by the single loop. No
  `std::atomic`, no locks anywhere.
- **Rejected**
  - *Thread pool for request handling* — forces a mutex around the key-store
    map and the token-bucket state on day one. The service is I/O-bound and
    each `/check` is microseconds of work; a pool mostly adds lock contention
    and latency.
  - *Thread-per-connection (blocking sockets)* — simpler parser, but shared
    key-store still needs locking, and connection counts in a load test would
    mean thousands of threads.
  - *Multi-reactor (one loop per core, `SO_REUSEPORT`)* — higher ceiling, but
    every core needs its own key store (then how do they agree?) or a shared
    locked one. In `redis` mode the shared state is already in Redis, so this
    buys little for the distributed path and a lot of complexity for the local
    path.
- **Revisit if** — Phase 4 load testing shows a single core saturated below
  the network limit. Interfaces are kept reactor-friendly: injectable clock,
  limiter behind an abstract interface, per-connection state owned by the
  connection object.

### 0.2 Rate-limit algorithms

- **Decision** — Ship two implementations behind a `RateLimiter` interface:
  `TokenBucketLimiter` and `SlidingWindowLimiter` (sliding-window *counter*,
  the two-bucket weighted approximation). Both use **lazy refill / lazy
  decay**: state carries a timestamp, and elapsed time is folded in on each
  `check()` call. No background timer touches limiter state.
- **Rejected**
  - *Fixed-window counter* — trivial, but allows up to 2× the limit across a
    window boundary (full burst at the end of window N, full burst at the
    start of N+1). The sliding-window counter removes that for one extra
    multiply.
  - *Sliding-window log (store every hit timestamp)* — exact, but memory per
    key grows with the limit and with traffic; a hot key at limit 10 000
    holds 10 000 timestamps. Unbounded per-key memory is a DoS vector.
  - *Timer-driven refill (a scheduled event per key that adds tokens)* — a
    wakeup per active key per tick; scheduler load scales with key count, and
    idle keys still cost timers. Lazy refill is O(1) per request and costs
    nothing for idle keys.
  - *GCRA / leaky bucket as the only algorithm* — GCRA is elegant and we may
    add it, but token bucket is the one interviewers ask to see derived, and
    sliding window is the one they ask to compare against.

### 0.3 Clock injection

- **Decision** — Abstract `Clock` with `now() -> steady time point`.
  `SystemClock` wraps `std::chrono::steady_clock`; `ManualClock` is advanced
  explicitly in tests. Every limiter takes a `Clock&`.
- **Rejected**
  - *Call `steady_clock::now()` directly in the limiters* — refill and window
    behaviour then can't be asserted exactly; tests become `sleep`-based and
    flaky.
  - *Templating the limiter on a clock type* — header bloat and error-message
    noise for a dependency that is injected once at construction.

### 0.4 Memory bounding / key eviction

- **Decision** — The key store is a bounded `unordered_map` plus an intrusive
  LRU list. On insert past `max_keys`, evict the least-recently-used entry. A
  periodic sweep (from the event loop's timer) also drops entries idle longer
  than `idle_ttl`. Both bounds are config.
- **Rejected**
  - *Unbounded map* — memory grows with the number of distinct keys ever
    seen; an attacker varying the key (random IP, random `?key=`) drives the
    process OOM.
  - *Per-key expiry timer* — one timer per key; same scaling problem as
    timer-driven refill.
  - *Fixed-size hash with modulo bucketing (no eviction, collisions share a
    bucket)* — collisions mean two unrelated keys share a limit; silent and
    hard to debug.

### 0.5 Key extraction

- **Decision** — `KeyExtractor` turns a request into a limit key. Configurable
  source: `query` (`?key=`), `header` (a named header, e.g.
  `X-Api-Key`), or `ip` (peer address, honouring `X-Forwarded-For` left-most
  entry only when `trust_forwarded_for` is set). Missing key → configurable:
  reject with 400, or fall back to `ip`.
- **Rejected**
  - *Always the peer IP* — useless behind a load balancer where every request
    arrives from the same address.
  - *Trust `X-Forwarded-For` unconditionally* — trivially spoofable; only
    honoured behind a trusted hop, gated by config.

### 0.6 Redis integration and the blocking-client problem

- **Decision** — `redis` mode uses a **synchronous** `hiredis` connection with
  an aggressive timeout (`redis.timeout_ms`, default 50). The `/check` handler
  makes one blocking `EVALSHA` call. On timeout, connection error, or Redis
  returning an error, the limiter **fails open** (request allowed) and a
  counter is bumped. A reconnect-with-backoff loop re-establishes a dropped
  connection.
- **Rejected**
  - *hiredis async adapter bound to our epoll loop* — the correct end state:
    Redis I/O multiplexed on the same loop, `/check` suspends the connection
    until the reply arrives. It turns every handler into a continuation and is
    ~a day of work on its own. Documented as the known limitation and the
    next step, not built in the 2-day sprint.
  - *Dedicated Redis I/O thread + request queue* — removes the stall from the
    loop, but adds a queue, a thread, and cross-thread wakeups — the shared
    state we spent 0.1 avoiding.
  - *Fail closed on Redis failure* — a Redis blip then returns 429 to every
    client. For a rate limiter, availability of the protected service matters
    more than a brief window of over-admission; fail-open is the default,
    `redis.fail_closed` flips it.
- **Revisit if** — the sprint has slack, or a later pass: replace with the
  async adapter.

### 0.7 Redis atomicity — why Lua

- **Decision** — The token-bucket update in `redis` mode is a single Lua
  script: read `tokens` + `last_refill` from a hash, compute refill from
  elapsed time, test-and-decrement, write back with `PEXPIRE`. Run via
  `EVALSHA`; on `NOSCRIPT` load it with `SCRIPT LOAD` / `EVAL` and retry.
- **Rejected**
  - *`GET` then `SET` from the client* — two round trips with a gap; two
    instances both read 1 token left, both allow, both write 0. Classic
    read-modify-write race.
  - *`INCR` + `EXPIRE` fixed window* — atomic per command, but `EXPIRE` can
    land on a key that `INCR` just recreated after expiry, and it's a fixed
    window with the boundary-burst problem from 0.2.
  - *`WATCH`/`MULTI`/`EXEC` optimistic transaction* — works, but retries under
    contention on hot keys and is more client code than one script.
  - *Redis 7 `CLIENT NO-EVICT` / server-side functions (`FUNCTION`)* — nicer
    packaging, but raises the minimum Redis version for no behavioural gain
    over `EVALSHA`.

### 0.8 CI

- **Decision** — GitHub Actions, `ubuntu-24.04`, **gcc-13**. Matrix over build
  config only: `debug` (ASan + UBSan) and `release` (-O3). A `lint` job runs
  `clang-format --dry-run --Werror`. Redis for the Phase 3 tests comes from
  the `redis:7` service container.
- **Rejected**
  - *gcc + clang matrix* — the two find different UB and it's worth it for a
    rigor-judged project, but deferred; the codebase is small and
    single-threaded.
  - *ThreadSanitizer job* — nothing for it to find in single-threaded code.

### 0.9 Dependency acquisition

- **Decision** — CMake `FetchContent` with pinned Git tags for
  `nlohmann/json`, `Catch2`, `hiredis`;
  `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=ALWAYS` so a correctly-versioned system
  package is used when present. Redis *server* is a Phase 3 test/runtime
  dependency, not a build dependency.
- **Rejected**
  - *System packages only* — Catch2 v2-vs-v3 and json version skew across
    distros means "works on my machine" for a reviewer.
  - *git submodules* — `--recursive` is forgotten; detached-HEAD confusion.
  - *vcpkg / Conan* — a manifest, lockfile, and toolchain file for three
    trivially-buildable dependencies.

### 0.10 Error handling convention

- **Decision** — Exceptions only during startup (config parse, socket bind,
  listen, initial Redis connect when `fail_closed`). On the hot path (accept /
  read / parse / check / respond) errors are values: a project
  `rl::core::Errc` enum with its own `std::error_category`
  (`would_block`, `closed_by_peer`, `connection_reset`, `timed_out`,
  `protocol_error`, `redis_unavailable`, ...).
- **Rejected**
  - *Exceptions throughout* — throwing across an epoll callback on every peer
    reset is expensive and obscures control flow.
  - *Bespoke `Result<T,E>`* — `std::expected` is C++23 (not in our baseline);
    a hand-rolled version is extra surface. `std::error_code` is standard and
    integrates with `<system_error>`.

### 0.11 Warning flags

- **Decision** — `-Wall -Wextra -Wpedantic -Wshadow`. `-Werror` in CI only
  (`RL_WERROR`, OFF for local builds), carried on an `INTERFACE` target that
  only our own targets link; fetched dependencies compile without it.
- **Rejected**
  - *`-Wconversion` / `-Wsign-conversion`* — catches the `size_t`/`ssize_t`
    length-arithmetic bugs a parser/buffer codebase is prone to; may be added
    once the buffer/parser code is stable.
  - *`-Werror` locally* — blocks iteration on a stray warning mid-edit.

### 0.12 Configuration input

- **Decision** — A JSON config file (`nlohmann/json`) plus CLI overrides:
  `--config <path>`, `--listen <host:port>`, `--mode local|redis`,
  `--redis <host:port>`. Config covers: listen address, limiter algorithm and
  parameters (`rate`, `burst`, `window_ms`), key-store bounds (`max_keys`,
  `idle_ttl_ms`), key extraction, and the Redis block.
- **Rejected**
  - *CLI flags only* — the limiter and key-extraction settings are structured;
    flags for all of it are unpleasant and error-prone.
  - *A second format (YAML/TOML)* — another dependency; `nlohmann/json` is
    already allowed.

### 0.13 Namespace

- **Decision** — `rl::` root, with sub-namespaces `rl::core`, `rl::net`,
  `rl::http`, `rl::limit`, `rl::redis`.
- **Rejected** — `ratelimiter::` (longer, no benefit); a flat `rl::` (loses
  the module-to-namespace correspondence that documents the headers).

### 0.14 Formatting

- **Decision** — `.clang-format`, LLVM base, 4-space indent, 100 columns. A
  `lint` CI job runs `clang-format --dry-run --Werror`. No `clang-tidy`.
- **Rejected** — *`clang-tidy` from day one* — a curated check set is its own
  tuning exercise and would generate noise on scaffolding code.

### 0.15 Repository layout

- **Decision** — Single `rl_core` static library, modules under
  `src/<module>/` with headers co-located next to their `.cpp` (no separate
  `include/` tree). `src/main.cpp` builds the `ratelimiter` executable;
  `tests/` mirrors `src/` and links `rl_core` + Catch2. `bench/` holds `wrk`
  scripts (not a build target).
- **Rejected**
  - *Flat `src/`* — no subsystem boundaries.
  - *Separate public `include/rl/` tree* — install/export ceremony for an ABI
    surface this project does not need.
  - *One static library per module* — circular headers (net <-> http) and
    slower builds with no runtime benefit.

### 0.16 Build presets

- **Decision** — `CMakePresets.json` with `debug` (Debug, `-Og -g`,
  ASan + UBSan, `-fno-sanitize-recover=all`) and `release` (Release, `-O3`,
  IPO/LTO guarded by `check_ipo_supported`, `-DNDEBUG`). CI presets
  `ci-debug` / `ci-release` inherit these and add `RL_WERROR=ON`. Generator:
  Ninja. Separate build trees per preset.
- **Rejected** — Ninja Multi-Config / single build tree: harder to reason
  about for tooling and sanitizer isolation.

### 0.17 `/check` response contract

- **Decision** — `GET /check` with the key taken per 0.5. Allowed →
  `200 OK`; denied → `429 Too Many Requests` with `Retry-After: <seconds>`.
  Both carry a JSON body: `{"allowed":bool, "limit":int, "remaining":int,
  "reset_ms":int}` where `reset_ms` is time until the bucket/window would next
  admit. `GET /healthz` → `200` `"ok"`. `GET /stats` → JSON counters
  (requests, allowed, denied, evictions, redis_failopen, key_count).
- **Rejected**
  - *`POST /check` with a JSON body* — a GET with a query key is cache-key
    friendly, trivial to call from `wrk`, and the check has no side effect the
    caller supplies a body for.
  - *Return only the status code, no body* — the `remaining`/`reset` fields
    are what a caller needs to backoff intelligently; omitting them pushes
    every caller to guess.

---

## Open questions carried into later phases

### Q-P1.1 — epoll level-triggered vs edge-triggered

To be settled when Phase 1 (the HTTP service loop) is built: readiness
semantics, the drain-until-`EAGAIN` obligation of ET, `EPOLLONESHOT`,
starvation/fairness under load, and the interaction with partial reads and
writes. Default lean: level-triggered for the sprint (simpler correctness),
ET noted as the tuning lever for Phase 4.

---

## Scope constraints

- **Phase 2** — HTTP request bodies are not read (`/check` is a GET). Requests
  with a body are drained and ignored up to a cap, then the connection is
  closed.
- **Phase 2** — HTTP/1.1 only. No HTTP/2, no TLS (terminate TLS at a front
  proxy in a real deployment; out of scope here).
- **Phase 3** — One Redis node. Redis Cluster / sentinel failover is out of
  scope.
