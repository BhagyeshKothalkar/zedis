#include "hash_table.h"

#include <string.h>

static uint32_t ht_hash(const char *key, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)key[i];
    h *= 16777619u;
  }
  return h;
}

int zedis_ht_init(zedis_hash_table_t *ht, zedis_arena_t *arena,
                  size_t capacity) {
  if (ht == NULL || arena == NULL || capacity == 0) {
    return -1;
  }

  ht->entries = zedis_arena_calloc(arena, capacity, sizeof(ht_entry_t), 64);
  if (ht->entries == NULL) {
    return -1;
  }

  ht->capacity = capacity;
  ht->count = 0;
  return 0;
}

void zedis_ht_destroy(zedis_hash_table_t *ht) {
  if (ht == NULL) {
    return;
  }
  memset(ht, 0, sizeof(*ht));
}

static int entry_key_match(const ht_entry_t *e, uint32_t hash, const char *key,
                           size_t key_len) {
  return e->state == HT_OCCUPIED && e->hash == hash && e->key_len == key_len &&
         memcmp(e->key, key, key_len) == 0;
}

int zedis_ht_set(zedis_hash_table_t *ht, const char *key, size_t key_len,
                 const char *value, size_t value_len) {
  if (key_len > ZEDIS_HT_MAX_KEY || value_len > ZEDIS_HT_MAX_VALUE) {
    return -1;
  }

  if (ht->count * 10 >= ht->capacity * 7) {
    return -1; /* fixed capacity, no rehash */
  }

  uint32_t hash = ht_hash(key, key_len);
  size_t idx = (size_t)hash % ht->capacity;
  uint32_t psl = 0;

  ht_entry_t incoming;
  memset(&incoming, 0, sizeof(incoming));
  incoming.hash = hash;
  incoming.key_len = key_len;
  incoming.value_len = value_len;
  memcpy(incoming.key, key, key_len);
  memcpy(incoming.value, value, value_len);

  for (;;) {
    ht_entry_t *e = &ht->entries[idx];

    if (e->state == HT_EMPTY || e->state == HT_TOMBSTONE) {
      e->state = HT_OCCUPIED;
      e->psl = psl;
      e->hash = incoming.hash;
      e->key_len = incoming.key_len;
      e->value_len = incoming.value_len;
      memcpy(e->key, incoming.key, incoming.key_len);
      memcpy(e->value, incoming.value, incoming.value_len);
      ht->count++;
      return 0;
    }

    if (entry_key_match(e, hash, key, key_len)) {
      e->value_len = value_len;
      memcpy(e->value, value, value_len);
      return 0;
    }

    if (psl > e->psl) {
      ht_entry_t tmp = *e;
      e->state = HT_OCCUPIED;
      e->psl = psl;
      e->hash = incoming.hash;
      e->key_len = incoming.key_len;
      e->value_len = incoming.value_len;
      memcpy(e->key, incoming.key, incoming.key_len);
      memcpy(e->value, incoming.value, incoming.value_len);
      incoming = tmp;
      psl = tmp.psl;
    }

    psl++;
    idx = (idx + 1) % ht->capacity;
  }
}

int zedis_ht_get(const zedis_hash_table_t *ht, const char *key, size_t key_len,
                 char *value_out, size_t value_cap, size_t *value_len_out) {
  if (ht->capacity == 0) {
    return -1;
  }

  uint32_t hash = ht_hash(key, key_len);
  size_t idx = (size_t)hash % ht->capacity;
  uint32_t psl = 0;

  for (;;) {
    const ht_entry_t *e = &ht->entries[idx];

    if (e->state == HT_EMPTY) {
      return -1;
    }

    if (e->state == HT_OCCUPIED && e->hash == hash && e->key_len == key_len &&
        memcmp(e->key, key, key_len) == 0) {
      if (value_out != NULL && value_cap > 0) {
        size_t copy = e->value_len < value_cap ? e->value_len : value_cap - 1;
        memcpy(value_out, e->value, copy);
        value_out[copy] = '\0';
      }
      if (value_len_out != NULL) {
        *value_len_out = e->value_len;
      }
      return 0;
    }

    if (psl > e->psl) {
      return -1;
    }

    psl++;
    idx = (idx + 1) % ht->capacity;
  }
}

int zedis_ht_del(zedis_hash_table_t *ht, const char *key, size_t key_len) {
  uint32_t hash = ht_hash(key, key_len);
  size_t idx = (size_t)hash % ht->capacity;
  uint32_t psl = 0;

  for (;;) {
    ht_entry_t *e = &ht->entries[idx];

    if (e->state == HT_EMPTY) {
      return -1;
    }

    if (entry_key_match(e, hash, key, key_len)) {
      e->state = HT_TOMBSTONE;
      e->psl = 0;
      ht->count--;
      return 0;
    }

    if (psl > e->psl) {
      return -1;
    }

    psl++;
    idx = (idx + 1) % ht->capacity;
  }
}
