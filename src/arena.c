#define _GNU_SOURCE
#include "arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static size_t align_up(size_t value, size_t align) {
  return (value + align - 1) & ~(align - 1);
}

int zedis_arena_init(zedis_arena_t *arena, size_t size) {
  if (arena == NULL || size == 0) {
    return -1;
  }

  memset(arena, 0, sizeof(*arena));

  size_t map_size = align_up(size, 2 * 1024 * 1024);

  void *mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  if (mem != MAP_FAILED) {
    arena->used_hugepages = 1;
  } else {
    mem = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
      return -1;
    }
  }

  if (madvise(mem, map_size, MADV_HUGEPAGE) != 0) {
    /* Best-effort THP hint when explicit hugepages are unavailable. */
  }

  arena->base = (uint8_t *)mem;
  arena->size = map_size;
  arena->offset = 0;
  return 0;
}

void zedis_arena_destroy(zedis_arena_t *arena) {
  if (arena == NULL || arena->base == NULL) {
    return;
  }

  munmap(arena->base, arena->size);
  memset(arena, 0, sizeof(*arena));
}

void *zedis_arena_alloc(zedis_arena_t *arena, size_t size, size_t align) {
  if (arena == NULL || size == 0) {
    return NULL;
  }

  if (align == 0) {
    align = 64;
  }

  size_t start = align_up(arena->offset, align);
  if (start + size > arena->size) {
    return NULL;
  }

  arena->offset = start + size;
  return arena->base + start;
}

void *zedis_arena_calloc(zedis_arena_t *arena, size_t count, size_t size,
                         size_t align) {
  size_t total = count * size;
  void *ptr = zedis_arena_alloc(arena, total, align);
  if (ptr != NULL) {
    memset(ptr, 0, total);
  }
  return ptr;
}
