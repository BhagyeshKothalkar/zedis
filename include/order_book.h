#ifndef ZEDIS_ORDER_BOOK_H
#define ZEDIS_ORDER_BOOK_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

typedef struct price_level {
  int64_t bid_qty;
  int64_t ask_qty;
} price_level_t;

typedef struct zedis_order_book {
  price_level_t *levels;
  int price_min;
  int price_max;
  size_t num_levels;
} zedis_order_book_t;

int zedis_book_init(zedis_order_book_t *book, zedis_arena_t *arena,
                    int price_min, int price_max);
void zedis_book_destroy(zedis_order_book_t *book);

int zedis_book_bid(zedis_order_book_t *book, int price, int64_t qty);
int zedis_book_ask(zedis_order_book_t *book, int price, int64_t qty);
int zedis_book_level(const zedis_order_book_t *book, int price,
                     int64_t *bid_out, int64_t *ask_out);

#endif /* ZEDIS_ORDER_BOOK_H */
