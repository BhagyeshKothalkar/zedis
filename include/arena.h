#ifndef ZEDIS_ARENA_H
#define ZEDIS_ARENA_H

#include <stddef.h>
#include <stdint.h>

#define ZEDIS_HUGEPAGE_SIZE (1024ULL * 1024ULL * 1024ULL)     /* 1 GiB */
#define ZEDIS_DEFAULT_ARENA_SIZE (256ULL * 1024ULL * 1024ULL) /* 256 MiB */

typedef struct zedis_arena {
  uint8_t *base;
  size_t size;
  size_t offset;
  int used_hugepages;
} zedis_arena_t;

int zedis_arena_init(zedis_arena_t *arena, size_t size);
void zedis_arena_destroy(zedis_arena_t *arena);

void *zedis_arena_alloc(zedis_arena_t *arena, size_t size, size_t align);
void *zedis_arena_calloc(zedis_arena_t *arena, size_t count, size_t size,
                         size_t align);

#endif /* ZEDIS_ARENA_H */
