#include "ring_buffer.h"

#include <stdatomic.h>
#include <string.h>

static size_t next_power_of_two(size_t v) {
    if (v < 2) {
        return 2;
    }
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

int zedis_ring_init(zedis_ring_buffer_t *ring, zedis_arena_t *arena, size_t capacity) {
    if (ring == NULL || arena == NULL || capacity == 0) {
        return -1;
    }

    size_t cap = next_power_of_two(capacity);
    ring->slots = zedis_arena_calloc(arena, cap, sizeof(ring_slot_t), 64);
    if (ring->slots == NULL) {
        return -1;
    }

    ring->capacity = cap;
    atomic_store(&ring->write_seq, 0);
    atomic_store(&ring->read_seq, 0);
    return 0;
}

void zedis_ring_destroy(zedis_ring_buffer_t *ring) {
    if (ring == NULL) {
        return;
    }
    memset(ring, 0, sizeof(*ring));
}

int zedis_ring_publish(zedis_ring_buffer_t *ring, const char *channel,
                       const char *payload, size_t payload_len) {
    if (channel == NULL || payload == NULL) {
        return -1;
    }

    if (payload_len > ZEDIS_RING_MSG_MAX) {
        return -1;
    }

    uint64_t seq = atomic_load_explicit(&ring->write_seq, memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&ring->read_seq, memory_order_acquire);
    if (seq - read >= ring->capacity) {
        return -1;
    }

    size_t idx = (size_t)(seq & (ring->capacity - 1));

    ring_slot_t *slot = &ring->slots[idx];
    size_t ch_len = strlen(channel);
    if (ch_len >= sizeof(slot->channel)) {
        ch_len = sizeof(slot->channel) - 1;
    }

    memcpy(slot->channel, channel, ch_len);
    slot->channel[ch_len] = '\0';
    memcpy(slot->payload, payload, payload_len);
    slot->payload_len = payload_len;
    slot->seq = seq;

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&ring->write_seq, seq + 1, memory_order_release);
    return 0;
}

int zedis_ring_poll(zedis_ring_buffer_t *ring, ring_slot_t *out) {
    uint64_t read = atomic_load_explicit(&ring->read_seq, memory_order_relaxed);
    uint64_t write = atomic_load_explicit(&ring->write_seq, memory_order_acquire);

    if (read >= write) {
        return -1;
    }

    size_t idx = (size_t)(read & (ring->capacity - 1));
    if (out != NULL) {
        *out = ring->slots[idx];
    }
    atomic_store_explicit(&ring->read_seq, read + 1, memory_order_relaxed);
    return 0;
}
