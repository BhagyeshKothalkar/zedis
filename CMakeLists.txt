cmake_minimum_required(VERSION 3.16)
project(zedis C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

set(ZEDIS_SOURCES
    src/main.c
    src/server.c
    src/event_loop.c
    src/conn.c
    src/resp.c
    src/arena.c
    src/slab.c
    src/hash_table.c
    src/order_book.c
    src/ring_buffer.c
    src/btree.c
    src/append_log.c
    src/key_registry.c
    src/affinity.c
)

add_executable(zedis ${ZEDIS_SOURCES})
target_include_directories(zedis PRIVATE include)
install(TARGETS zedis RUNTIME DESTINATION bin)

add_executable(zedis_test tests/test_ds.c
    src/arena.c
    src/slab.c
    src/hash_table.c
    src/order_book.c
    src/ring_buffer.c
    src/btree.c
    src/append_log.c
    src/key_registry.c
)
target_include_directories(zedis_test PRIVATE include)

add_executable(zedis_test_resp tests/test_resp.c
    src/resp.c
    src/slab.c
    src/arena.c
)
target_include_directories(zedis_test_resp PRIVATE include)

option(ZEDIS_GPROF "Build with gprof profiling flags" OFF)
if(ZEDIS_GPROF)
    target_compile_options(zedis PRIVATE -pg)
    target_link_options(zedis PRIVATE -pg)
endif()

enable_testing()
add_test(NAME ds_unit COMMAND zedis_test)
add_test(NAME resp_unit COMMAND zedis_test_resp)
