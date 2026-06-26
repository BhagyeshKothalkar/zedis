#ifndef ZEDIS_RING_BUFFER_H
#define ZEDIS_RING_BUFFER_H

#include "arena.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define ZEDIS_RING_MSG_MAX 512

typedef struct ring_slot {
  char channel[64];
  char payload[ZEDIS_RING_MSG_MAX];
  size_t payload_len;
  uint64_t seq;
} ring_slot_t;

typedef struct zedis_ring_buffer {
  ring_slot_t *slots;
  size_t capacity;
  _Atomic uint64_t write_seq;
  _Atomic uint64_t read_seq;
} zedis_ring_buffer_t;

int zedis_ring_init(zedis_ring_buffer_t *ring, zedis_arena_t *arena,
                    size_t capacity);
void zedis_ring_destroy(zedis_ring_buffer_t *ring);

int zedis_ring_publish(zedis_ring_buffer_t *ring, const char *channel,
                       const char *payload, size_t payload_len);
int zedis_ring_poll(zedis_ring_buffer_t *ring, ring_slot_t *out);

#endif /* ZEDIS_RING_BUFFER_H */
