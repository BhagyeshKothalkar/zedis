#ifndef ZEDIS_SLAB_H
#define ZEDIS_SLAB_H

#include "arena.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define ZEDIS_SLAB_64 64
#define ZEDIS_SLAB_256 256
#define ZEDIS_SLAB_512 512
#define ZEDIS_SLAB_4096 4096

typedef struct slab_block {
  struct slab_block *next;
  uint8_t data[];
} slab_block_t;

typedef struct zedis_slab_pool {
  size_t block_size;
  size_t block_count;
  _Atomic(slab_block_t *) free_list;
  uint8_t *storage;
} zedis_slab_pool_t;

typedef struct zedis_slab {
  zedis_slab_pool_t pools[4];
} zedis_slab_t;

int zedis_slab_init(zedis_slab_t *slab, zedis_arena_t *arena, size_t count_64,
                    size_t count_256, size_t count_512, size_t count_4096);
void zedis_slab_destroy(zedis_slab_t *slab);

void *zedis_slab_alloc(zedis_slab_t *slab, size_t size);
void zedis_slab_free(zedis_slab_t *slab, void *ptr, size_t size);

#endif /* ZEDIS_SLAB_H */
