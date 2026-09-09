#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build"
PORT=16379
ZEDIS="${BUILD}/zedis"
REDIS_CLI="${REDIS_CLI:-redis-cli}"

cd "${ROOT}"

# Background process state.
ZEDIS_PID=""
VG_PID=""

SERVER_LOG=""
VG_LOG=""

###############################################################################
# Process helpers
###############################################################################

cleanup_process() {
    local pid="${1:-}"
    local name="${2:-process}"

    [[ -z "${pid}" ]] && return 0

    # If the process has already exited, reap it if possible.
    if ! kill -0 "${pid}" 2>/dev/null; then
        wait "${pid}" 2>/dev/null || true
        return 0
    fi

    echo "==> Cleaning up ${name} (pid ${pid})"

    kill -TERM "${pid}" 2>/dev/null || true

    # Give it a short, bounded grace period.
    for _ in $(seq 1 50); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait "${pid}" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done

    echo "WARNING: ${name} did not exit after SIGTERM; sending SIGKILL" >&2
    kill -KILL "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}

cleanup() {
    # Never allow cleanup itself to terminate the script unexpectedly.
    local status=$?
    set +e

    cleanup_process "${VG_PID}" "Valgrind/Zedis"
    cleanup_process "${ZEDIS_PID}" "Zedis"

    [[ -n "${SERVER_LOG}" ]] && rm -f "${SERVER_LOG}"
    [[ -n "${VG_LOG}" ]] && rm -f "${VG_LOG}"

    exit "${status}"
}

trap cleanup EXIT

###############################################################################
# Readiness
###############################################################################

wait_for_port() {
    local port="$1"
    local pid="$2"
    local label="$3"
    local timeout_seconds="${4:-10}"

    echo "==> Waiting for ${label} on port ${port}"

    local deadline=$((SECONDS + timeout_seconds))

    while ((SECONDS < deadline)); do
        # Detect an early server exit immediately.
        if ! kill -0 "${pid}" 2>/dev/null; then
            echo "ERROR: ${label} exited before becoming ready" >&2
            return 1
        fi

        # Bound redis-cli itself. A broken client/server must never make this
        # readiness loop block indefinitely.
        if timeout 1s "${REDIS_CLI}" -p "${port}" PING >/dev/null 2>&1; then
            echo "==> ${label} is ready"
            return 0
        fi

        sleep 0.1
    done

    echo "ERROR: ${label} did not become ready within ${timeout_seconds}s" >&2
    return 1
}

###############################################################################
# Bounded shutdown + status collection
###############################################################################

stop_and_wait() {
    local pid="$1"
    local name="$2"
    local log_file="${3:-}"

    echo "==> Stopping ${name} (pid ${pid})"

    if ! kill -0 "${pid}" 2>/dev/null; then
        # Already exited. Reap it and return its status.
        set +e
        wait "${pid}"
        local status=$?
        set -e
        return "${status}"
    fi

    kill -TERM "${pid}" 2>/dev/null || true

    # IMPORTANT:
    # Do not use `wait` as the timeout mechanism. `wait` can block forever.
    # First wait for the process to disappear, then reap it.
    for _ in $(seq 1 50); do
        if ! kill -0 "${pid}" 2>/dev/null; then
            set +e
            wait "${pid}"
            local status=$?
            set -e

            echo "==> ${name} exited with status ${status}"

            return "${status}"
        fi

        sleep 0.1
    done

    echo "ERROR: ${name} did not exit within 5 seconds of SIGTERM" >&2

    echo "==> Process information" >&2
    ps -o pid,ppid,pgid,sid,stat,wchan:32,cmd -p "${pid}" >&2 || true

    echo "==> Child processes" >&2
    pgrep -a -P "${pid}" >&2 || true

    echo "==> Process tree" >&2
    pstree -ap "${pid}" >&2 || true

    echo "==> Wait channel" >&2
    if [[ -r "/proc/${pid}/wchan" ]]; then
        cat "/proc/${pid}/wchan" >&2 || true
    fi

    if [[ -n "${log_file}" && -f "${log_file}" ]]; then
        echo "==> ${name} output" >&2
        cat "${log_file}" >&2 || true
    fi

    echo "ERROR: sending SIGKILL to ${name}" >&2
    kill -KILL "${pid}" 2>/dev/null || true

    set +e
    wait "${pid}"
    set -e

    return 1
}

###############################################################################
# Build
###############################################################################

echo "==> Building zedis"
cmake -B "${BUILD}" -DCMAKE_BUILD_TYPE=Debug
cmake --build "${BUILD}"

###############################################################################
# Unit tests
###############################################################################

echo "==> Running unit tests"
"${BUILD}/zedis_test"
"${BUILD}/zedis_test_resp"

###############################################################################
# Valgrind unit tests
###############################################################################

echo "==> Valgrind memory check on unit tests"

valgrind \
    --error-exitcode=1 \
    --leak-check=full \
    --show-leak-kinds=all \
    "${BUILD}/zedis_test"

valgrind \
    --error-exitcode=1 \
    --leak-check=full \
    --show-leak-kinds=all \
    "${BUILD}/zedis_test_resp"

###############################################################################
# Normal integration server
###############################################################################

echo "==> Starting zedis on port ${PORT}"

SERVER_LOG="$(mktemp)"

"${ZEDIS}" \
    --port "${PORT}" \
    --no-busy-poll \
    >"${SERVER_LOG}" 2>&1 &

ZEDIS_PID=$!

if ! wait_for_port "${PORT}" "${ZEDIS_PID}" "zedis" 10; then
    echo "==> zedis startup output" >&2
    cat "${SERVER_LOG}" >&2 || true
    exit 1
fi

run() {
    "${REDIS_CLI}" -p "${PORT}" "$@"
}

###############################################################################
# Integration smoke tests
###############################################################################

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

###############################################################################
# Stop normal integration server
###############################################################################

if ! stop_and_wait "${ZEDIS_PID}" "Zedis" "${SERVER_LOG}"; then
    echo "ERROR: normal Zedis server failed to shut down cleanly" >&2
    exit 1
fi

ZEDIS_PID=""
rm -f "${SERVER_LOG}"
SERVER_LOG=""

###############################################################################
# Valgrind server test
###############################################################################

echo "==> Valgrind on zedis server (short workload)"

VG_PORT=$((PORT + 1))
VG_LOG="$(mktemp)"

valgrind \
    --error-exitcode=1 \
    --leak-check=full \
    --show-leak-kinds=all \
    "${ZEDIS}" \
    --port "${VG_PORT}" \
    --no-busy-poll \
    >"${VG_LOG}" 2>&1 &

VG_PID=$!

if ! wait_for_port "${VG_PORT}" "${VG_PID}" "Valgrind/Zedis" 10; then
    echo "==> Valgrind/Zedis startup output" >&2
    cat "${VG_LOG}" >&2 || true
    exit 1
fi

###############################################################################
# Valgrind workload
###############################################################################

"${REDIS_CLI}" -p "${VG_PORT}" PING >/dev/null
"${REDIS_CLI}" -p "${VG_PORT}" SET vg 1 >/dev/null
"${REDIS_CLI}" -p "${VG_PORT}" ZADD z 1 m >/dev/null
"${REDIS_CLI}" -p "${VG_PORT}" LPUSH l x >/dev/null

###############################################################################
# Valgrind shutdown
###############################################################################

if ! stop_and_wait "${VG_PID}" "Valgrind/Zedis" "${VG_LOG}"; then
    echo "ERROR: Valgrind/Zedis shutdown failed" >&2
    exit 1
fi

VG_PID=""

###############################################################################
# Final result
###############################################################################

echo "==> All tests passed"
