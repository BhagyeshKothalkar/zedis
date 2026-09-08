# Stage 1: Build
FROM ubuntu:24.04 AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

# Stage 2: Runtime
FROM debian:bookworm-slim

COPY --from=builder /app/build/zedis /usr/local/bin/zedis

EXPOSE 16379

ENTRYPOINT ["/usr/local/bin/zedis", "--port", "16379", "--no-busy-poll"]
