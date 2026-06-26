#include "order_book.h"

#include <string.h>

static int price_to_index(const zedis_order_book_t *book, int price) {
  if (price < book->price_min || price > book->price_max) {
    return -1;
  }
  return price - book->price_min;
}

int zedis_book_init(zedis_order_book_t *book, zedis_arena_t *arena,
                    int price_min, int price_max) {
  if (book == NULL || arena == NULL || price_max < price_min) {
    return -1;
  }

  size_t num = (size_t)(price_max - price_min + 1);
  book->levels = zedis_arena_calloc(arena, num, sizeof(price_level_t), 64);
  if (book->levels == NULL) {
    return -1;
  }

  book->price_min = price_min;
  book->price_max = price_max;
  book->num_levels = num;
  return 0;
}

void zedis_book_destroy(zedis_order_book_t *book) {
  if (book == NULL) {
    return;
  }
  memset(book, 0, sizeof(*book));
}

int zedis_book_bid(zedis_order_book_t *book, int price, int64_t qty) {
  int idx = price_to_index(book, price);
  if (idx < 0) {
    return -1;
  }

  book->levels[idx].bid_qty += qty;
  if (book->levels[idx].bid_qty < 0) {
    book->levels[idx].bid_qty = 0;
  }
  return 0;
}

int zedis_book_ask(zedis_order_book_t *book, int price, int64_t qty) {
  int idx = price_to_index(book, price);
  if (idx < 0) {
    return -1;
  }

  book->levels[idx].ask_qty += qty;
  if (book->levels[idx].ask_qty < 0) {
    book->levels[idx].ask_qty = 0;
  }
  return 0;
}

int zedis_book_level(const zedis_order_book_t *book, int price,
                     int64_t *bid_out, int64_t *ask_out) {
  int idx = price_to_index(book, price);
  if (idx < 0) {
    return -1;
  }

  if (bid_out != NULL) {
    *bid_out = book->levels[idx].bid_qty;
  }
  if (ask_out != NULL) {
    *ask_out = book->levels[idx].ask_qty;
  }
  return 0;
}
