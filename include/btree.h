#ifndef ZEDIS_BTREE_H
#define ZEDIS_BTREE_H

#include <stddef.h>
#include <stdint.h>

#include "arena.h"

#define ZEDIS_BTREE_MAX_MEMBER 48
#define ZEDIS_BTREE_MAX_ENTRIES 4096
#define ZEDIS_BTREE_NODE_SIZE 64

/*
 * Cache-line aligned B-tree node (exactly 64 bytes).
 * Internal nodes: keys[3] + children[4] + header.
 * Leaf nodes:     keys[3] + values[4]  + header.
 */
typedef struct btree_node {
  int64_t keys[3];
  union {
    uint32_t children[4];
    uint32_t values[4];
  };
  uint8_t is_leaf;
  uint8_t count;
  uint16_t _pad;
} __attribute__((aligned(64))) btree_node_t;

typedef struct btree_member {
  char data[ZEDIS_BTREE_MAX_MEMBER];
  size_t len;
  int64_t score;
} btree_member_t;

typedef struct zedis_btree {
  btree_node_t *nodes;
  btree_member_t *members;
  uint32_t root;
  uint32_t free_node;
  uint32_t node_count;
  uint32_t member_count;
  uint32_t max_nodes;
  uint32_t max_members;
} zedis_btree_t;

int zedis_btree_init(zedis_btree_t *tree, zedis_arena_t *arena,
                     uint32_t max_members);
void zedis_btree_destroy(zedis_btree_t *tree);

int zedis_btree_add(zedis_btree_t *tree, int64_t score, const char *member,
                    size_t member_len);
int zedis_btree_score(const zedis_btree_t *tree, const char *member,
                      size_t member_len, int64_t *score_out);
int zedis_btree_range(const zedis_btree_t *tree, int64_t start_idx,
                      int64_t stop_idx, char *reply, size_t reply_cap,
                      int *reply_len);

#endif /* ZEDIS_BTREE_H */
