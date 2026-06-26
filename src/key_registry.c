#include "key_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lookup_index(const zedis_hash_table_t *ht, const char *key,
                        size_t key_len, uint32_t *idx_out) {
  char buf[32];
  size_t len = 0;
  if (zedis_ht_get(ht, key, key_len, buf, sizeof(buf), &len) != 0) {
    return -1;
  }
  *idx_out = (uint32_t)strtoul(buf, NULL, 10);
  return 0;
}

static int store_index(zedis_hash_table_t *ht, const char *key, size_t key_len,
                       uint32_t idx) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%u", idx);
  if (n <= 0) {
    return -1;
  }
  return zedis_ht_set(ht, key, key_len, buf, (size_t)n);
}

int zedis_zset_reg_init(zedis_zset_registry_t *reg, zedis_arena_t *arena,
                        uint32_t max_zsets, uint32_t max_members_per_zset) {
  if (reg == NULL || arena == NULL || max_zsets == 0) {
    return -1;
  }

  memset(reg, 0, sizeof(*reg));
  reg->max_zsets = max_zsets;

  reg->trees = zedis_arena_calloc(arena, max_zsets, sizeof(zedis_btree_t), 64);
  if (reg->trees == NULL) {
    return -1;
  }

  for (uint32_t i = 0; i < max_zsets; i++) {
    if (zedis_btree_init(&reg->trees[i], arena, max_members_per_zset) != 0) {
      return -1;
    }
  }

  if (zedis_ht_init(&reg->keys, arena, max_zsets * 2) != 0) {
    return -1;
  }

  return 0;
}

void zedis_zset_reg_destroy(zedis_zset_registry_t *reg) {
  if (reg == NULL) {
    return;
  }
  memset(reg, 0, sizeof(*reg));
}

static int zset_index_for_key(zedis_zset_registry_t *reg, const char *key,
                              size_t key_len, uint32_t *idx_out, int create) {
  uint32_t idx = 0;
  if (lookup_index(&reg->keys, key, key_len, &idx) == 0) {
    *idx_out = idx;
    return 0;
  }

  if (!create) {
    return -1;
  }

  if (reg->used >= reg->max_zsets) {
    return -1;
  }

  idx = reg->used++;
  if (store_index(&reg->keys, key, key_len, idx) != 0) {
    return -1;
  }

  *idx_out = idx;
  return 0;
}

int zedis_zset_reg_add(zedis_zset_registry_t *reg, const char *key,
                       size_t key_len, int64_t score, const char *member,
                       size_t member_len) {
  uint32_t idx = 0;
  if (zset_index_for_key(reg, key, key_len, &idx, 1) != 0) {
    return -1;
  }
  return zedis_btree_add(&reg->trees[idx], score, member, member_len);
}

int zedis_zset_reg_score(const zedis_zset_registry_t *reg, const char *key,
                         size_t key_len, const char *member, size_t member_len,
                         int64_t *score_out) {
  uint32_t idx = 0;
  if (lookup_index(&reg->keys, key, key_len, &idx) != 0) {
    return -1;
  }
  return zedis_btree_score(&reg->trees[idx], member, member_len, score_out);
}

int zedis_zset_reg_range(const zedis_zset_registry_t *reg, const char *key,
                         size_t key_len, int64_t start, int64_t stop,
                         char *reply, size_t reply_cap, int *reply_len) {
  uint32_t idx = 0;
  if (lookup_index(&reg->keys, key, key_len, &idx) != 0) {
    int n = snprintf(reply, reply_cap, "*0\r\n");
    if (n <= 0 || (size_t)n >= reply_cap) {
      return -1;
    }
    *reply_len = n;
    return 0;
  }
  return zedis_btree_range(&reg->trees[idx], start, stop, reply, reply_cap,
                           reply_len);
}

int zedis_list_reg_init(zedis_list_registry_t *reg, zedis_arena_t *arena,
                        uint32_t max_lists) {
  if (reg == NULL || arena == NULL || max_lists == 0) {
    return -1;
  }

  memset(reg, 0, sizeof(*reg));
  reg->max_lists = max_lists;

  reg->lists = zedis_arena_calloc(arena, max_lists, sizeof(list_meta_t), 64);
  if (reg->lists == NULL) {
    return -1;
  }

  for (uint32_t i = 0; i < max_lists; i++) {
    reg->lists[i].head_off = ZEDIS_AOL_NO_PREV;
    reg->lists[i].tail_off = ZEDIS_AOL_NO_PREV;
  }

  if (zedis_ht_init(&reg->keys, arena, max_lists * 2) != 0) {
    return -1;
  }

  return 0;
}

void zedis_list_reg_destroy(zedis_list_registry_t *reg) {
  if (reg == NULL) {
    return;
  }
  memset(reg, 0, sizeof(*reg));
}

static int list_index_for_key(zedis_list_registry_t *reg, const char *key,
                              size_t key_len, uint32_t *idx_out, int create) {
  uint32_t idx = 0;
  if (lookup_index(&reg->keys, key, key_len, &idx) == 0) {
    *idx_out = idx;
    return 0;
  }

  if (!create) {
    return -1;
  }

  if (reg->used >= reg->max_lists) {
    return -1;
  }

  idx = reg->used++;
  if (store_index(&reg->keys, key, key_len, idx) != 0) {
    return -1;
  }

  *idx_out = idx;
  return 0;
}

int zedis_list_reg_lpush(zedis_list_registry_t *reg, zedis_append_log_t *log,
                         const char *key, size_t key_len, const char *val,
                         size_t val_len) {
  uint32_t idx = 0;
  if (list_index_for_key(reg, key, key_len, &idx, 1) != 0) {
    return -1;
  }
  return zedis_aol_lpush(log, &reg->lists[idx], key, key_len, val, val_len);
}

int zedis_list_reg_lrange(const zedis_list_registry_t *reg,
                          zedis_append_log_t *log, const char *key,
                          size_t key_len, int64_t start, int64_t stop,
                          char *reply, size_t reply_cap, int *reply_len) {
  uint32_t idx = 0;
  if (lookup_index(&reg->keys, key, key_len, &idx) != 0) {
    int n = snprintf(reply, reply_cap, "*0\r\n");
    if (n <= 0 || (size_t)n >= reply_cap) {
      return -1;
    }
    *reply_len = n;
    return 0;
  }
  return zedis_aol_lrange(log, &reg->lists[idx], start, stop, reply, reply_cap,
                          reply_len);
}

int zedis_list_reg_llen(const zedis_list_registry_t *reg, const char *key,
                        size_t key_len, uint32_t *len_out) {
  uint32_t idx = 0;
  if (lookup_index(&reg->keys, key, key_len, &idx) != 0) {
    if (len_out != NULL) {
      *len_out = 0;
    }
    return 0;
  }
  if (len_out != NULL) {
    *len_out = zedis_aol_llen(&reg->lists[idx]);
  }
  return 0;
}
