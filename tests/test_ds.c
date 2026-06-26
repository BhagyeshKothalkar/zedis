#include "append_log.h"
#include "arena.h"
#include "btree.h"
#include "hash_table.h"
#include "key_registry.h"
#include "order_book.h"
#include "ring_buffer.h"
#include "slab.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
} while (0)

static void test_hash_table(void) {
    zedis_arena_t arena;
    ASSERT(zedis_arena_init(&arena, 4 * 1024 * 1024) == 0, "arena init");

    zedis_hash_table_t ht;
    ASSERT(zedis_ht_init(&ht, &arena, 64) == 0, "ht init");
    ASSERT(zedis_ht_set(&ht, "foo", 3, "bar", 3) == 0, "ht set");

    char val[64];
    size_t len = 0;
    ASSERT(zedis_ht_get(&ht, "foo", 3, val, sizeof(val), &len) == 0, "ht get");
    ASSERT(len == 3 && memcmp(val, "bar", 3) == 0, "ht value");

    ASSERT(zedis_ht_del(&ht, "foo", 3) == 0, "ht del");
    ASSERT(zedis_ht_get(&ht, "foo", 3, val, sizeof(val), &len) != 0, "ht miss after del");

    zedis_ht_destroy(&ht);
    zedis_arena_destroy(&arena);
}

static void test_slab(void) {
    zedis_arena_t arena;
    ASSERT(zedis_arena_init(&arena, 1024 * 1024) == 0, "arena init");

    zedis_slab_t slab;
    ASSERT(zedis_slab_init(&slab, &arena, 16, 16, 8, 4) == 0, "slab init");

    void *a = zedis_slab_alloc(&slab, 64);
    void *b = zedis_slab_alloc(&slab, 256);
    ASSERT(a != NULL && b != NULL, "slab alloc");

    zedis_slab_free(&slab, a, 64);
    zedis_slab_free(&slab, b, 256);

    void *c = zedis_slab_alloc(&slab, 64);
    ASSERT(c != NULL, "slab reuse");

    zedis_slab_destroy(&slab);
    zedis_arena_destroy(&arena);
}

static void test_btree(void) {
    zedis_arena_t arena;
    ASSERT(zedis_arena_init(&arena, 4 * 1024 * 1024) == 0, "arena init");

    zedis_btree_t tree;
    ASSERT(zedis_btree_init(&tree, &arena, 128) == 0, "btree init");

    ASSERT(zedis_btree_add(&tree, 10, "alpha", 5) == 0, "btree add alpha");
    ASSERT(zedis_btree_add(&tree, 20, "beta", 4) == 0, "btree add beta");
    ASSERT(zedis_btree_add(&tree, 5, "gamma", 5) == 0, "btree add gamma");

    int64_t score = 0;
    ASSERT(zedis_btree_score(&tree, "beta", 4, &score) == 0, "btree score");
    ASSERT(score == 20, "btree score value");

    char reply[4096];
    int rlen = 0;
    ASSERT(zedis_btree_range(&tree, 0, -1, reply, sizeof(reply), &rlen) == 0, "btree range");
    ASSERT(strstr(reply, "gamma") != NULL, "range has gamma");
    ASSERT(strstr(reply, "alpha") != NULL, "range has alpha");
    ASSERT(strstr(reply, "beta") != NULL, "range has beta");

    /* Test score update and ordering preservation */
    ASSERT(zedis_btree_add(&tree, 30, "alpha", 5) == 0, "btree update alpha");
    ASSERT(zedis_btree_score(&tree, "alpha", 5, &score) == 0, "btree score alpha");
    ASSERT(score == 30, "btree score value updated");

    rlen = 0;
    memset(reply, 0, sizeof(reply));
    ASSERT(zedis_btree_range(&tree, 0, -1, reply, sizeof(reply), &rlen) == 0, "btree range after update");

    const char *expected = "*3\r\n$5\r\ngamma\r\n$4\r\nbeta\r\n$5\r\nalpha\r\n";
    ASSERT(strcmp(reply, expected) == 0, "range order is correct after update");

    zedis_btree_destroy(&tree);
    zedis_arena_destroy(&arena);
}

static void test_append_log(void) {
    zedis_append_log_t log;
    ASSERT(zedis_aol_init(&log, 1024 * 1024, NULL) == 0, "aol init");

    list_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.head_off = ZEDIS_AOL_NO_PREV;
    meta.tail_off = ZEDIS_AOL_NO_PREV;
    ASSERT(zedis_aol_lpush(&log, &meta, "mylist", 6, "one", 3) == 0, "lpush one");
    ASSERT(zedis_aol_lpush(&log, &meta, "mylist", 6, "two", 3) == 0, "lpush two");
    ASSERT(zedis_aol_llen(&meta) == 2, "llen");

    char reply[4096];
    int rlen = 0;
    ASSERT(zedis_aol_lrange(&log, &meta, 0, -1, reply, sizeof(reply), &rlen) == 0, "lrange");
    ASSERT(strstr(reply, "two") != NULL, "lrange has two");
    ASSERT(strstr(reply, "one") != NULL, "lrange has one");

    zedis_aol_destroy(&log);
}

static void test_ring_buffer(void) {
    zedis_arena_t arena;
    ASSERT(zedis_arena_init(&arena, 1024 * 1024) == 0, "arena init");

    zedis_ring_buffer_t ring;
    ASSERT(zedis_ring_init(&ring, &arena, 8) == 0, "ring init");
    ASSERT(zedis_ring_publish(&ring, "ticks", "msg1", 4) == 0, "ring publish");

    ring_slot_t slot;
    ASSERT(zedis_ring_poll(&ring, &slot) == 0, "ring poll");
    ASSERT(slot.payload_len == 4 && memcmp(slot.payload, "msg1", 4) == 0, "ring payload");

    zedis_ring_destroy(&ring);
    zedis_arena_destroy(&arena);
}

static void test_order_book(void) {
    zedis_arena_t arena;
    ASSERT(zedis_arena_init(&arena, 1024 * 1024) == 0, "arena init");

    zedis_order_book_t book;
    ASSERT(zedis_book_init(&book, &arena, 100, 110) == 0, "book init");
    ASSERT(zedis_book_bid(&book, 105, 100) == 0, "book bid");
    ASSERT(zedis_book_ask(&book, 105, 50) == 0, "book ask");

    int64_t bid = 0;
    int64_t ask = 0;
    ASSERT(zedis_book_level(&book, 105, &bid, &ask) == 0, "book level");
    ASSERT(bid == 100 && ask == 50, "book quantities");

    zedis_book_destroy(&book);
    zedis_arena_destroy(&arena);
}

static void test_key_registry(void) {
    zedis_arena_t arena;
    ASSERT(zedis_arena_init(&arena, 8 * 1024 * 1024) == 0, "arena init");

    zedis_zset_registry_t zsets;
    ASSERT(zedis_zset_reg_init(&zsets, &arena, 4, 32) == 0, "zset reg init");
    ASSERT(zedis_zset_reg_add(&zsets, "z", 1, 1, "a", 1) == 0, "zadd via reg");

    int64_t score = 0;
    ASSERT(zedis_zset_reg_score(&zsets, "z", 1, "a", 1, &score) == 0, "zscore via reg");
    ASSERT(score == 1, "zscore value");

    zedis_append_log_t log;
    ASSERT(zedis_aol_init(&log, 1024 * 1024, NULL) == 0, "aol init");
    zedis_list_registry_t lists;
    ASSERT(zedis_list_reg_init(&lists, &arena, 4) == 0, "list reg init");
    ASSERT(zedis_list_reg_lpush(&lists, &log, "l", 1, "x", 1) == 0, "lpush via reg");

    uint32_t len = 0;
    ASSERT(zedis_list_reg_llen(&lists, "l", 1, &len) == 0, "llen via reg");
    ASSERT(len == 1, "llen value");

    zedis_aol_destroy(&log);
    zedis_zset_reg_destroy(&zsets);
    zedis_list_reg_destroy(&lists);
    zedis_arena_destroy(&arena);
}

int main(void) {
    test_hash_table();
    test_slab();
    test_btree();
    test_append_log();
    test_ring_buffer();
    test_order_book();
    test_key_registry();

    printf("Ran %d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
