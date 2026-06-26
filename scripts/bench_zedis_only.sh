#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ZEDIS_PORT=16579

cd "${ROOT}"
mkdir -p ./benchmarks

# Track Docker Container IDs for strict runtime cleanup
ZEDIS_CID=""

cleanup() { 
    echo "==> Cleaning up Docker containers..."
    if [ -n "${ZEDIS_CID}" ]; then
        docker kill "${ZEDIS_CID}" >/dev/null 2>&1 || true
        docker rm "${ZEDIS_CID}" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

# Helper for engine benchmarks using redis-benchmark
bench_cmd() {
    redis-benchmark -p "${ZEDIS_PORT}" -n 20000 -c 10 "$@" 2>/dev/null | awk '
    /throughput summary:/ { rps=$3 }
    /latency summary/ { flag=1; next }
    flag && /^[[:space:]]*[0-9]/ {
        p50=$3; p95=$4; p99=$5;
        print rps " rps | p50=" p50 " ms | p95=" p95 " ms | p99=" p99 " ms";
        exit;
    }
    '
}

# --- 1. START ZEDIS CONTAINER ---
echo "==> Starting Zedis Docker Container..."
ZEDIS_CID=$(docker run -d -p "${ZEDIS_PORT}:${ZEDIS_PORT}" zedis:latest --port "${ZEDIS_PORT}" --no-busy-poll 2>/dev/null || \
            docker run -d -p "${ZEDIS_PORT}:${ZEDIS_PORT}" zedis:latest)

until redis-cli -p "${ZEDIS_PORT}" PING >/dev/null 2>&1; do
    sleep 0.005
done

# --- 2. RUN ENGINE BENCHMARKS ---
echo "==> Running Core Structure Benchmarks on Zedis..."

# Strings
SUMMARY_SET_16B=$(bench_cmd -t set -d 16)
SUMMARY_SET_1KB=$(bench_cmd -t set -d 1024)

# 3KB payload to leave headroom in the 4KB connection write buffer
VAL_LIMIT=$(head -c3072 </dev/zero | tr "\0" x)
SUMMARY_SET_3KB=$(bench_cmd SET huge "${VAL_LIMIT}")

# Populate key:000000000000 for GET benchmark to verify real GET speed
redis-cli -p "${ZEDIS_PORT}" SET key:000000000000 "${VAL_LIMIT}" >/dev/null
SUMMARY_GET_3KB=$(bench_cmd GET key:000000000000)

# Sorted Sets
echo "Populating ZSET (1k items)..."
seq 1 1000 | awk '{print "ZADD benchzset " $1 " member" $1}' | redis-cli -p "${ZEDIS_PORT}" --pipe >/dev/null

SUMMARY_ZSCORE=$(bench_cmd ZSCORE benchzset member500)
SUMMARY_ZRANGE_10=$(bench_cmd ZRANGE benchzset 0 9)
SUMMARY_ZRANGE_200=$(bench_cmd ZRANGE benchzset 0 199)
SUMMARY_ZADD_EXIST=$(bench_cmd ZADD benchzset 42 member42)
SUMMARY_ZADD_NEW=$(bench_cmd ZADD benchzset 1001 newmember)

# Lists
echo "Populating LIST (1k items)..."
seq 1 1000 | awk '{print "LPUSH benchlist " $1}' | redis-cli -p "${ZEDIS_PORT}" --pipe >/dev/null

SUMMARY_LPUSH=$(bench_cmd LPUSH benchlist x)
SUMMARY_LRANGE_10=$(bench_cmd LRANGE benchlist 0 9)
SUMMARY_LRANGE_200=$(bench_cmd LRANGE benchlist 0 199)

# Order Book
echo "Populating Order Book..."
(
    for i in $(seq 1 5000); do echo "BID $i 1"; done
    for i in $(seq 1 5000); do echo "ASK $((5000+i)) 1"; done
) | redis-cli -p "${ZEDIS_PORT}" --pipe >/dev/null

SUMMARY_BID=$(bench_cmd BID 2500 5)
SUMMARY_ASK=$(bench_cmd ASK 7500 5)

# --- 3. WRITE SUMMARY REPORT ---
SUMMARY_FILE="./benchmarks/zedis_only_summary.txt"

{
    echo "========================================================================="
    echo "                      ZEDIS ENGINE BENCHMARK REPORT                      "
    echo "========================================================================="
    printf "%-20s : %s\n" "SET 16B" "${SUMMARY_SET_16B}"
    printf "%-20s : %s\n" "SET 1KB" "${SUMMARY_SET_1KB}"
    printf "%-20s : %s\n" "SET 3KB" "${SUMMARY_SET_3KB}"
    printf "%-20s : %s\n" "GET 3KB" "${SUMMARY_GET_3KB}"
    printf "%-20s : %s\n" "ZSCORE" "${SUMMARY_ZSCORE}"
    printf "%-20s : %s\n" "ZRANGE 10" "${SUMMARY_ZRANGE_10}"
    printf "%-20s : %s\n" "ZRANGE 200" "${SUMMARY_ZRANGE_200}"
    printf "%-20s : %s\n" "ZADD existing" "${SUMMARY_ZADD_EXIST}"
    printf "%-20s : %s\n" "ZADD new" "${SUMMARY_ZADD_NEW}"
    printf "%-20s : %s\n" "LPUSH" "${SUMMARY_LPUSH}"
    printf "%-20s : %s\n" "LRANGE 10" "${SUMMARY_LRANGE_10}"
    printf "%-20s : %s\n" "LRANGE 200" "${SUMMARY_LRANGE_200}"
    printf "%-20s : %s\n" "BID (Order Book)" "${SUMMARY_BID}"
    printf "%-20s : %s\n" "ASK (Order Book)" "${SUMMARY_ASK}"
    echo "========================================================================="
} > "${SUMMARY_FILE}"

cat "${SUMMARY_FILE}"
