#!/usr/bin/env bash
# Runs kvbench against kvserver in each storage mode and prints a Markdown
# table. Usage:  bench/run_bench.sh <build-dir> [--quick]
#
#   serial  one fsync per write, reads wait for it (the original design)
#   group   rwlock + group commit (default)
#   nosync  group without fsync -- not durable, shows the cost of fsync
#
# Each run uses a fresh server and an empty WAL in a temp directory, so the
# disk the temp directory lives on is what gets measured. Every cell is the
# median of REPEAT runs (default 3; --quick: 1 run, 10x fewer requests).
set -euo pipefail

BUILD=${1:?usage: $0 <build-dir> [--quick]}
QUICK=${2:-}
SERVER=$(realpath "$BUILD/kvserver")
BENCH=$(realpath "$BUILD/kvbench")
PORT=${PORT:-19999}
TOTAL_OPS=20000
REPEAT=${REPEAT:-3}
if [[ "$QUICK" == "--quick" ]]; then TOTAL_OPS=2000; REPEAT=1; fi

WORK=$(mktemp -d)
SERVER_PID=""
cleanup() {
    [[ -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

start_server() {   # $1 = mode
    rm -f "$WORK/kv.log"
    (cd "$WORK" && exec "$SERVER" -p "$PORT" -m "$1" >/dev/null 2>&1) &
    SERVER_PID=$!
    for _ in $(seq 100); do
        (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && return 0
        sleep 0.05
    done
    echo "server did not start" >&2
    exit 1
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
}

echo "| workload | clients | mode | ops/s | SET p50 / p99 (µs) | GET p50 / p99 (µs) |"
echo "|---|---:|---|---:|---:|---:|"

run() {   # $1 = workload label, $2 = get%, $3 = clients
    local label=$1 get=$2 clients=$3
    local per=$(( TOTAL_OPS / clients ))
    (( per < 100 )) && per=100
    for mode in serial group nosync; do
        # REPEAT runs, keep the one with the median throughput (small VMs are noisy)
        local results=()
        for _ in $(seq "$REPEAT"); do
            start_server "$mode"
            results+=("$("$BENCH" -p "$PORT" -c "$clients" -n "$per" -r "$get" -q)")
            stop_server
        done
        read -r tput s50 s99 g50 g99 < <(printf '%s\n' "${results[@]}" | sort -n | sed -n "$(( (REPEAT + 1) / 2 ))p")
        local set_col="—" get_col="—"
        (( get < 100 )) && set_col="$s50 / $s99"
        (( get > 0 ))   && get_col="$g50 / $g99"
        echo "| $label | $clients | $mode | $tput | $set_col | $get_col |"
    done
}

for c in 1 8 64 256; do run "100% SET" 0 "$c"; done
for c in 1 8 64 256; do run "100% GET" 100 "$c"; done
for c in 8 64 256;   do run "90% GET / 10% SET" 90 "$c"; done
