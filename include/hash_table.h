#ifndef ZEDIS_HASH_TABLE_H
#define ZEDIS_HASH_TABLE_H

#include "arena.h"

#include <stddef.h>
#include <stdint.h>

#define ZEDIS_HT_MAX_KEY 256
#define ZEDIS_HT_MAX_VALUE 4096

typedef enum ht_state { HT_EMPTY = 0, HT_OCCUPIED, HT_TOMBSTONE } ht_state_t;

typedef struct ht_entry {
  ht_state_t state;
  uint32_t psl;
  uint32_t hash;
  size_t key_len;
  size_t value_len;
  char key[ZEDIS_HT_MAX_KEY];
  char value[ZEDIS_HT_MAX_VALUE];
} ht_entry_t;

typedef struct zedis_hash_table {
  ht_entry_t *entries;
  size_t capacity;
  size_t count;
} zedis_hash_table_t;

int zedis_ht_init(zedis_hash_table_t *ht, zedis_arena_t *arena,
                  size_t capacity);
void zedis_ht_destroy(zedis_hash_table_t *ht);

int zedis_ht_set(zedis_hash_table_t *ht, const char *key, size_t key_len,
                 const char *value, size_t value_len);
int zedis_ht_get(const zedis_hash_table_t *ht, const char *key, size_t key_len,
                 char *value_out, size_t value_cap, size_t *value_len_out);
int zedis_ht_del(zedis_hash_table_t *ht, const char *key, size_t key_len);

#endif /* ZEDIS_HASH_TABLE_H */
