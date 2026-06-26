#ifndef ZEDIS_KEY_REGISTRY_H
#define ZEDIS_KEY_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#include "append_log.h"
#include "arena.h"
#include "btree.h"
#include "hash_table.h"

typedef struct zedis_zset_registry {
  zedis_btree_t *trees;
  zedis_hash_table_t keys;
  uint32_t max_zsets;
  uint32_t used;
} zedis_zset_registry_t;

typedef struct zedis_list_registry {
  list_meta_t *lists;
  zedis_hash_table_t keys;
  uint32_t max_lists;
  uint32_t used;
} zedis_list_registry_t;

int zedis_zset_reg_init(zedis_zset_registry_t *reg, zedis_arena_t *arena,
                        uint32_t max_zsets, uint32_t max_members_per_zset);
void zedis_zset_reg_destroy(zedis_zset_registry_t *reg);
int zedis_zset_reg_add(zedis_zset_registry_t *reg, const char *key,
                       size_t key_len, int64_t score, const char *member,
                       size_t member_len);
int zedis_zset_reg_score(const zedis_zset_registry_t *reg, const char *key,
                         size_t key_len, const char *member, size_t member_len,
                         int64_t *score_out);
int zedis_zset_reg_range(const zedis_zset_registry_t *reg, const char *key,
                         size_t key_len, int64_t start, int64_t stop,
                         char *reply, size_t reply_cap, int *reply_len);

int zedis_list_reg_init(zedis_list_registry_t *reg, zedis_arena_t *arena,
                        uint32_t max_lists);
void zedis_list_reg_destroy(zedis_list_registry_t *reg);
int zedis_list_reg_lpush(zedis_list_registry_t *reg, zedis_append_log_t *log,
                         const char *key, size_t key_len, const char *val,
                         size_t val_len);
int zedis_list_reg_lrange(const zedis_list_registry_t *reg,
                          zedis_append_log_t *log, const char *key,
                          size_t key_len, int64_t start, int64_t stop,
                          char *reply, size_t reply_cap, int *reply_len);
int zedis_list_reg_llen(const zedis_list_registry_t *reg, const char *key,
                        size_t key_len, uint32_t *len_out);

#endif /* ZEDIS_KEY_REGISTRY_H */
