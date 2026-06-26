#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
PORT=16379
ZEDIS="${BUILD}/zedis"
REDIS_CLI="${REDIS_CLI:-redis-cli}"

cd "${ROOT}"

echo "==> Building zedis"
cmake -B "${BUILD}" -DCMAKE_BUILD_TYPE=Debug
cmake --build "${BUILD}"

echo "==> Running unit tests"
"${BUILD}/zedis_test"
"${BUILD}/zedis_test_resp"

echo "==> Valgrind memory check on unit tests"
valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=all \
    "${BUILD}/zedis_test"
valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=all \
    "${BUILD}/zedis_test_resp"

wait_for_port() {
    local port=$1
    for _ in $(seq 1 50); do
        if "${REDIS_CLI}" -p "${port}" PING >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

echo "==> Starting zedis on port ${PORT}"
"${ZEDIS}" --port "${PORT}" --no-busy-poll &
ZEDIS_PID=$!
cleanup() { kill "${ZEDIS_PID}" 2>/dev/null || true; wait "${ZEDIS_PID}" 2>/dev/null || true; }
trap cleanup EXIT
wait_for_port "${PORT}"

run() {
    "${REDIS_CLI}" -p "${PORT}" "$@"
}

echo "==> Integration smoke tests"
test "$(run PING)" = "PONG"
test "$(run SET k v)" = "OK"
test "$(run GET k)" = "v"
test "$(run DEL k)" = "1"
test "$(run ZADD zs 1 member1)" = "1"
test "$(run ZSCORE zs member1)" = "1"
test "$(run ZRANGE zs 0 -1 | wc -l)" -ge 1
test "$(run LPUSH mylist a)" = "1"
test "$(run LPUSH mylist b)" = "2"
test "$(run LLEN mylist)" = "2"
test "$(run LRANGE mylist 0 -1 | wc -l)" -ge 1
test "$(run BID 100 10)" = "10"
test "$(run ASK 101 5)" = "5"

echo "==> Valgrind on zedis server (short workload)"
VG_PORT=$((PORT + 1))
valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=all \
    "${ZEDIS}" --port "${VG_PORT}" --no-busy-poll &
VG_PID=$!
wait_for_port "${VG_PORT}"
"${REDIS_CLI}" -p "${VG_PORT}" PING >/dev/null
"${REDIS_CLI}" -p "${VG_PORT}" SET vg 1 >/dev/null
"${REDIS_CLI}" -p "${VG_PORT}" ZADD z 1 m >/dev/null
"${REDIS_CLI}" -p "${VG_PORT}" LPUSH l x >/dev/null
kill "${VG_PID}" 2>/dev/null || true
wait "${VG_PID}" 2>/dev/null || true

echo "==> All tests passed"
