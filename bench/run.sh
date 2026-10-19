#!/usr/bin/env bash
# Load-test profiles for the rate limiter (Phase 4, threads: design 0.20).
#
# Usage:
#   bench/run.sh [host:port]      profiles A-D against an instance already listening
#   bench/run.sh scaling          start the server at threads = 1, 2, 4 and print a
#                                 Markdown table (needs a release build)
#
# Scaling-mode env:
#   RL_BIN        server binary            (default build/release/ratelimiter)
#   RL_THREADS    thread counts to try     (default "1 2 4")
#   RL_WRK_T      wrk threads              (default 4; keep wrk + server threads
#                                           below the core count, or the load
#                                           generator steals the server's CPU)
#   RL_DURATION   seconds per run          (default 10)
#   RL_REDIS      host:port; also run the redis-mode profile against it
# Requires: wrk and curl on PATH.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "${1:-}" == "scaling" ]]; then
    BIN="${RL_BIN:-${HERE}/../build/release/ratelimiter}"
    THREADS="${RL_THREADS:-1 2 4}"
    WRK_T="${RL_WRK_T:-4}"
    DUR="${RL_DURATION:-10}"
    PORT=18200
    URL="http://127.0.0.1:${PORT}"
    [[ -x "${BIN}" ]] || { echo "no server binary at ${BIN} (cmake --build --preset release)"; exit 1; }
    echo "cores: $(nproc)   wrk: -t${WRK_T} -c128 -d${DUR}s   server: ${BIN}"

    # $1 = label, $2 = threads, rest = wrk args. Prints one table row.
    run_wrk() {
        local label="$1" n="$2"; shift 2
        local out rps p50 p99
        out="$(wrk -t"${WRK_T}" -c128 -d"${DUR}s" --latency "$@")"
        rps="$(awk '/Requests\/sec/ {printf "%.0f", $2}' <<<"${out}")"
        p50="$(awk '$1 == "50%" {print $2}' <<<"${out}")"
        p99="$(awk '$1 == "99%" {print $2}' <<<"${out}")"
        printf '| %s | %s | %s | %s | %s |\n' "${label}" "${n}" "${rps}" "${p50}" "${p99}"
    }

    # $1 = threads, rest = extra server flags. Starts the server, waits for it.
    start_server() {
        local n="$1"; shift
        "${BIN}" --listen "127.0.0.1:${PORT}" --threads "${n}" "$@" 2>/dev/null &
        SERVER_PID=$!
        for _ in $(seq 50); do curl -sf "${URL}/healthz" >/dev/null && return; sleep 0.1; done
        echo "server did not start"; exit 1
    }
    stop_server() { kill -INT "${SERVER_PID}"; wait "${SERVER_PID}" 2>/dev/null || true; }

    echo
    echo "| profile | threads | req/s | p50 | p99 |"
    echo "|---|---:|---:|---:|---:|"
    for n in ${THREADS}; do
        start_server "${n}"
        wrk -t2 -c16 -d2s "${URL}/healthz" >/dev/null # warmup
        run_wrk "/healthz" "${n}" "${URL}/healthz"
        RL_KEYS=2000 run_wrk "/check, 2000 keys" "${n}" -s "${HERE}/check.lua" "${URL}"
        RL_KEYS=1 run_wrk "/check, 1 hot key" "${n}" -s "${HERE}/check.lua" "${URL}"
        stop_server
        if [[ -n "${RL_REDIS:-}" ]]; then
            start_server "${n}" --mode redis --redis "${RL_REDIS}"
            RL_KEYS=2000 run_wrk "/check, redis mode, 2000 keys" "${n}" -s "${HERE}/check.lua" "${URL}"
            stop_server
        fi
    done
    exit 0
fi

TARGET="${1:-127.0.0.1:8080}"
URL="http://${TARGET}"

echo "== warmup =="
wrk -t2 -c16 -d5s "${URL}/healthz" || true

echo
echo "== profile A: /healthz throughput (no limiter work) =="
wrk -t4 -c128 -d20s --latency "${URL}/healthz"

echo
echo "== profile B: /check, 1000 keys, generous limit =="
RL_KEYS=1000 wrk -t4 -c128 -d20s --latency -s "${HERE}/check.lua" "${URL}"

echo
echo "== profile C: /check, 10 keys, tight limit -> lots of 429s =="
RL_KEYS=10 wrk -t4 -c128 -d20s --latency -s "${HERE}/check.lua" "${URL}"

echo
echo "== profile D: connection churn (no keep-alive) =="
wrk -t4 -c128 -d20s -H 'Connection: close' --latency "${URL}/healthz"
