#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZEDIS_PORT=16579
REDIS_PORT=6579
REQUESTS=1000000
CLIENTS=100

cd "${ROOT}"
mkdir -p ./benchmarks

REDIS_BENCH="${REDIS_BENCH:-redis-benchmark}"

if ! command -v "${REDIS_BENCH}" >/dev/null 2>&1; then
    echo "redis-benchmark not found; skipping benchmark comparison"
    exit 0
fi

# Track Docker Container IDs for strict runtime cleanup
ZEDIS_CID=""
REDIS_CID=""

cleanup() { 
    echo "==> Cleaning up Docker containers..."
    if [ -n "${ZEDIS_CID}" ]; then
        docker kill "${ZEDIS_CID}" >/dev/null 2>&1 || true
        docker rm "${ZEDIS_CID}" >/dev/null 2>&1 || true
    fi
    if [ -n "${REDIS_CID}" ]; then
        docker kill "${REDIS_CID}" >/dev/null 2>&1 || true
        docker rm "${REDIS_CID}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

# --- 1. START & MEASURE ZEDIS CONTAINER ---
echo "==> Starting Zedis Docker Container..."
START_NS=$(date +%s%N)

ZEDIS_CID=$(docker run -d -p "${ZEDIS_PORT}:${ZEDIS_PORT}" zedis:latest --port "${ZEDIS_PORT}" --no-busy-poll 2>/dev/null || \
            docker run -d -p "${ZEDIS_PORT}:${ZEDIS_PORT}" zedis:latest)

until redis-cli -p "${ZEDIS_PORT}" PING >/dev/null 2>&1; do
    sleep 0.005
done

END_NS=$(date +%s%N)
ZEDIS_STARTUP_MS=$(( (END_NS - START_NS) / 1000000 ))


# --- 2. START & MEASURE REDIS CONTAINER (PERFORMANCE OPTIMIZED) ---
echo "==> Starting Redis Docker Container..."
START_REDIS_NS=$(date +%s%N)

REDIS_CID=$(docker run -d -p "${REDIS_PORT}:6379" redis:latest redis-server \
    --save "" \
    --appendonly no \
    --tcp-keepalive 0 \
    --protected-mode no \
    --maxclients 10000)

until redis-cli -p "${REDIS_PORT}" PING >/dev/null 2>&1; do
    sleep 0.005
done

END_REDIS_NS=$(date +%s%N)
REDIS_STARTUP_MS=$(( (END_REDIS_NS - START_REDIS_NS) / 1000000 ))


# --- 3. THROUGHPUT & LATENCY BENCHMARKS ---
run_bench() {
    local port=$1
    local label=$2
    echo "==> Running core profiles for ${label}..."
    
    "${REDIS_BENCH}" -p "${port}" -t ping,set,get -n "${REQUESTS}" -c "${CLIENTS}" -q 2>/dev/null > "./benchmarks/${label}_throughput.txt"
    "${REDIS_BENCH}" -p "${port}" -t ping -n 100000 -c "${CLIENTS}" --csv 2>/dev/null > "./benchmarks/${label}_latency.csv" || true
}

run_bench "${ZEDIS_PORT}" "zedis"
run_bench "${REDIS_PORT}" "redis"


# --- 4. COMPREHENSIVE SUMMARY REPORT ---
SUMMARY_FILE="./benchmarks/compare_summary.txt"

{
    echo "=================================================="
    echo "          CORE COMPARISON BENCHMARK REPORT        "
    echo "=================================================="
    echo ""
    echo "=== Startup Latency ==="
    echo "Zedis Container Boot : ${ZEDIS_STARTUP_MS} ms"
    echo "Redis Container Boot : ${REDIS_STARTUP_MS} ms"
    echo ""
    echo "=== Throughput (req/sec) ==="
    echo "[Zedis]"
    cat "./benchmarks/zedis_throughput.txt" || echo "No data"
    echo ""
    echo "[Standard Redis (Optimized)]"
    cat "./benchmarks/redis_throughput.txt" || echo "No data"
    echo ""
    echo "=== Latency Percentiles (PING) ==="
    echo "Metrics: \"Type\",\"Min\",\"p50\",\"p95\",\"p99\",\"Max\""
    if [ -f "./benchmarks/zedis_latency.csv" ]; then
        echo "[Zedis]"
        cat "./benchmarks/zedis_latency.csv" | tr -d '"' | awk -F, 'NR>1 {print "PING latency: P50=" $3 "ms, P95=" $4 "ms, P99=" $5 "ms"}' || cat "./benchmarks/zedis_latency.csv"
    fi
    if [ -f "./benchmarks/redis_latency.csv" ]; then
        echo "[Standard Redis (Optimized)]"
        cat "./benchmarks/redis_latency.csv" | tr -d '"' | awk -F, 'NR>1 {print "PING latency: P50=" $3 "ms, P95=" $4 "ms, P99=" $5 "ms"}' || cat "./benchmarks/redis_latency.csv"
    fi
    echo "=================================================="
} > "${SUMMARY_FILE}"

# Cleanup intermediate csv files
rm -f ./benchmarks/zedis_latency.csv ./benchmarks/redis_latency.csv

cat "${SUMMARY_FILE}"
