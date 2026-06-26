# Stage 1: Build
FROM ubuntu:24.04 AS builder
RUN apt-get update && apt-get install -y build-essential cmake
COPY . /app
WORKDIR /app
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Stage 2: Run
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y libstdc++6 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /app/build/zedis /usr/local/bin/zedis

EXPOSE 16379
ENTRYPOINT ["zedis", "--port", "16379", "--no-busy-poll"]