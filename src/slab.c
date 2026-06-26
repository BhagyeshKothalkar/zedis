#include "slab.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

static int init_pool(zedis_slab_pool_t *pool, zedis_arena_t *arena,
                     size_t block_size, size_t block_count) {
  pool->block_size = block_size;
  pool->block_count = block_count;
  atomic_store(&pool->free_list, NULL);

  size_t stride = block_size;
  pool->storage = zedis_arena_alloc(arena, stride * block_count, 64);
  if (pool->storage == NULL) {
    return -1;
  }

  slab_block_t *head = NULL;
  for (size_t i = 0; i < block_count; i++) {
    slab_block_t *block = (slab_block_t *)(pool->storage + i * stride);
    block->next = head;
    head = block;
  }
  atomic_store(&pool->free_list, head);

  return 0;
}

int zedis_slab_init(zedis_slab_t *slab, zedis_arena_t *arena, size_t count_64,
                    size_t count_256, size_t count_512, size_t count_4096) {
  if (slab == NULL || arena == NULL) {
    return -1;
  }

  memset(slab, 0, sizeof(*slab));

  if (init_pool(&slab->pools[0], arena, ZEDIS_SLAB_64, count_64) != 0) {
    return -1;
  }
  if (init_pool(&slab->pools[1], arena, ZEDIS_SLAB_256, count_256) != 0) {
    return -1;
  }
  if (init_pool(&slab->pools[2], arena, ZEDIS_SLAB_512, count_512) != 0) {
    return -1;
  }
  if (init_pool(&slab->pools[3], arena, ZEDIS_SLAB_4096, count_4096) != 0) {
    return -1;
  }

  return 0;
}

void zedis_slab_destroy(zedis_slab_t *slab) {
  if (slab == NULL) {
    return;
  }
  memset(slab, 0, sizeof(*slab));
}

static zedis_slab_pool_t *pool_for_size(zedis_slab_t *slab, size_t size) {
  if (size <= ZEDIS_SLAB_64) {
    return &slab->pools[0];
  }
  if (size <= ZEDIS_SLAB_256) {
    return &slab->pools[1];
  }
  if (size <= ZEDIS_SLAB_512) {
    return &slab->pools[2];
  }
  if (size <= ZEDIS_SLAB_4096) {
    return &slab->pools[3];
  }
  return NULL;
}

void *zedis_slab_alloc(zedis_slab_t *slab, size_t size) {
  zedis_slab_pool_t *pool = pool_for_size(slab, size);
  if (pool == NULL) {
    return NULL;
  }

  slab_block_t *head = atomic_load(&pool->free_list);
  while (head != NULL) {
    slab_block_t *next = head->next;
    if (atomic_compare_exchange_weak(&pool->free_list, &head, next)) {
      head->next = NULL;
      return head->data;
    }
  }

  return NULL;
}

void zedis_slab_free(zedis_slab_t *slab, void *ptr, size_t size) {
  if (ptr == NULL) {
    return;
  }

  zedis_slab_pool_t *pool = pool_for_size(slab, size);
  if (pool == NULL) {
    return;
  }

  slab_block_t *block =
      (slab_block_t *)((uint8_t *)ptr - offsetof(slab_block_t, data));
  slab_block_t *head = atomic_load(&pool->free_list);
  do {
    block->next = head;
  } while (!atomic_compare_exchange_weak(&pool->free_list, &head, block));
}
