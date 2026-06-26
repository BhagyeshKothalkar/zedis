# Zedis

Zedis is an ultra-low-latency, single-threaded, event-driven in-memory key-value database built from scratch in C11. It is designed to emulate a subset of Redis commands with highly optimized internal data structures, custom memory management (Arenas and Slabs), edge-triggered epoll networks, and thread affinity pinning.

---

## Key Features & Architecture

### ⚡ Custom Memory Management
*   **Arena Allocator**: Avoids frequent malloc overhead and heap fragmentation by pre-allocating a large contiguous virtual memory buffer for database structures.
*   **Slab Allocator**: A specialized buddy/slab allocator that partitions memory chunks into discrete size classes, delivering $O(1)$ allocation/deallocation speeds.

### 🌐 High-Performance Networking
*   **Edge-Triggered Epoll**: Uses Linux `epoll` with `EPOLLET` (edge-triggered) semantics and non-blocking I/O to handle concurrent client connections efficiently.
*   **Stateful RESP Streaming Parser**: Zero-copy RESP parser. Reads straight from the kernel into parser buffers, slices elements in place, and supports fragmented and pipelined socket reads.
*   **Core Pinning (Thread Affinity)**: Option to pin the main execution thread to a specific CPU core (`--core`) to minimize context switching latency.

### 💾 Persistence
*   **Append-Only Log (AOL)**: Writes operations to an append-only log backed by memory-mapped files (`mmap`), guaranteeing data durability across server restarts.

### 📊 Engine Data Structures
*   **Robin Hood Hash Table**: Fixed-capacity hash table using open addressing and Robin Hood hashing (backward shift deletion) to maintain $O(1)$ lookup times.
*   **B-Tree**: A high-fanout B-Tree backing the Sorted Set (`ZSET`) operations, sorted by score and key.
*   **Append-Log Lists**: Off-heap list representations backing `LIST` commands.
*   **Ring Buffer**: Lock-free ring buffer powering high-throughput Pub/Sub messaging.
*   **Limit-Order Book**: High-performance bidding order book engine supporting real-time `BID` and `ASK` trading commands.

---

## Feature Tour & Commands

Zedis supports a focused, high-performance command set over the RESP protocol.

### 1. Strings
*   `SET <key> <value>`: Store a string value (max size 4086 bytes).
*   `GET <key>`: Retrieve a string value.
*   `DEL <key>`: Delete a key.

### 2. Sorted Sets (ZSet)
*   `ZADD <key> <score> <member>`: Add or update a member score in a sorted set.
*   `ZSCORE <key> <member>`: Get score of a member.
*   `ZRANGE <key> <start> <stop>`: Query score-ordered members (max `279` elements per batch response to prevent socket write buffer overflow).

### 3. Lists
*   `LPUSH <key> <value>`: Push value onto head of a list.
*   `LLEN <key>`: Retrieve list length.
*   `LRANGE <key> <start> <stop>`: Query list elements (max `279` elements per response).

### 4. Limit-Order Book
*   `BID <price> <quantity>`: Submit a buying bid at a price level.
*   `ASK <price> <quantity>`: Submit a selling ask at a price level.

---

## Building and Running

### Prerequisites
*   CMake (>= 3.16)
*   GCC or Clang with C11 support
*   Linux OS (uses `epoll`)
*   Valgrind (for running memory leak tests)
*   Docker (optional, for benchmarking)

### Build Instructions
Build the project using CMake:

```bash
# Create build directory
mkdir -p build && cd build

# Configure build (Release mode recommended for performance)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Compile binaries
make
```

### Running the Server
Start the compiled `zedis` server executable:

```bash
# Run server on port 16579 using edge-triggered blocking epoll
./build/zedis --port 16579 --no-busy-poll

# Run server with thread pinned to CPU core 0
./build/zedis --port 16579 --core 0
```

#### Command Line Options
*   `--port <port>`: Port to listen on (default: `16379`)
*   `--core <core-id>`: CPU core to pin execution thread to
*   `--arena-size <bytes>`: Custom memory arena size (default: `64MiB`)
*   `--no-busy-poll`: Uses standard blocking `epoll_wait` (highly recommended for local development to avoid 100% CPU usage)

---

## Testing

Zedis contains unit test suites and integration smoke tests.

Run the full testing pipeline:
```bash
./scripts/run_tests.sh
```

### What `run_tests.sh` does:
1.  **Builds** the project in `Debug` configuration.
2.  **Runs Data Structure Unit Tests (`zedis_test`)**: Validates slab, hash table, btree, append log, ring buffer, order book, and key registry logic.
3.  **Runs RESP Parser Unit Tests (`zedis_test_resp`)**: Validates RESP formatting, basic parsers, arrays, and fragmented chunk processing.
4.  **Runs Valgrind Memory Validation**: Executes memory leak checks on both unit test executables.
5.  **Integration Smoke Tests**: Starts the `zedis` server and runs integration queries using standard `redis-cli`.
6.  **Valgrind Server Test**: Spawns the server under Valgrind to ensure zero memory leaks during execution.

---

## Benchmarks

A dedicated Docker-backed benchmark suite is included. It measures operations using `redis-benchmark` to capture real throughput and latency percentiles.

Run the benchmarks:
```bash
./scripts/bench_zedis_only.sh
```

The script runs throughput checks for all major data structures and outputs the results to `./benchmarks/zedis_only_summary.txt`.

### Benchmark Results
An example of results obtained on a standard development machine:

```text
=========================================================================
                      ZEDIS ENGINE BENCHMARK REPORT                      
=========================================================================
SET 16B              : 16906.17 rps | p50=0.423 ms | p95=1.191 ms | p99=2.567 ms
SET 1KB              : 18921.47 rps | p50=0.407 ms | p95=1.055 ms | p99=1.935 ms
SET 3KB              : 19762.85 rps | p50=0.391 ms | p95=1.039 ms | p99=1.975 ms
GET 3KB              : 19474.20 rps | p50=0.407 ms | p95=0.975 ms | p99=1.959 ms
ZSCORE               : 18083.18 rps | p50=0.439 ms | p95=1.079 ms | p99=1.959 ms
ZRANGE 10            : 18709.07 rps | p50=0.439 ms | p95=0.975 ms | p99=1.751 ms
ZRANGE 200           : 14224.75 rps | p50=0.591 ms | p95=1.031 ms | p99=1.359 ms
ZADD existing        : 11883.54 rps | p50=0.759 ms | p95=1.319 ms | p99=1.823 ms
ZADD new             : 9657.17 rps | p50=0.895 ms | p95=1.783 ms | p99=2.631 ms
LPUSH                : 18639.33 rps | p50=0.431 ms | p95=0.999 ms | p99=1.607 ms
LRANGE 10            : 19646.37 rps | p50=0.407 ms | p95=0.967 ms | p99=1.799 ms
LRANGE 200           : 15444.02 rps | p50=0.527 ms | p95=0.951 ms | p99=1.399 ms
BID (Order Book)     : 18957.35 rps | p50=0.415 ms | p95=1.039 ms | p99=1.887 ms
ASK (Order Book)     : 20470.83 rps | p50=0.383 ms | p95=0.959 ms | p99=1.911 ms
=========================================================================
```
