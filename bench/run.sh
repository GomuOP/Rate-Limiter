#!/usr/bin/env bash
# Load-test profiles for the rate limiter (Phase 4).
#
# Usage:  bench/run.sh [host:port]
# Requires: wrk on PATH, a ratelimiter instance already listening.
set -euo pipefail

TARGET="${1:-127.0.0.1:8080}"
URL="http://${TARGET}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
