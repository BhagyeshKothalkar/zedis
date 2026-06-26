#include "btree.h"

#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(btree_node_t) == ZEDIS_BTREE_NODE_SIZE,
               "btree node must be 64 bytes");

#define BTREE_MAX_KEYS 3

static uint32_t alloc_node(zedis_btree_t *tree) {
  if (tree->free_node >= tree->max_nodes) {
    return UINT32_MAX;
  }
  return tree->free_node++;
}

static uint32_t alloc_member(zedis_btree_t *tree, int64_t score,
                             const char *member, size_t member_len) {
  if (tree->member_count >= tree->max_members) {
    return UINT32_MAX;
  }

  uint32_t idx = tree->member_count++;
  btree_member_t *m = &tree->members[idx];
  m->score = score;
  m->len = member_len < ZEDIS_BTREE_MAX_MEMBER ? member_len
                                               : ZEDIS_BTREE_MAX_MEMBER - 1;
  memcpy(m->data, member, m->len);
  m->data[m->len] = '\0';
  return idx;
}

static int member_equal(const btree_member_t *m, const char *member,
                        size_t member_len) {
  return m->len == member_len && memcmp(m->data, member, member_len) == 0;
}

static int score_member_cmp(int64_t score_a, const btree_member_t *a,
                            int64_t score_b, const char *member_b,
                            size_t len_b) {
  if (score_a != score_b) {
    return score_a < score_b ? -1 : 1;
  }
  size_t n = a->len < len_b ? a->len : len_b;
  int rc = memcmp(a->data, member_b, n);
  if (rc != 0) {
    return rc;
  }
  if (a->len < len_b) {
    return -1;
  }
  if (a->len > len_b) {
    return 1;
  }
  return 0;
}

static int find_in_leaf(const zedis_btree_t *tree, const btree_node_t *node,
                        const char *member, size_t member_len,
                        uint32_t *midx_out) {
  for (uint8_t i = 0; i < node->count; i++) {
    const btree_member_t *m = &tree->members[node->values[i]];
    if (member_equal(m, member, member_len)) {
      if (midx_out != NULL) {
        *midx_out = node->values[i];
      }
      return 0;
    }
  }
  return -1;
}

static int find_member(const zedis_btree_t *tree, uint32_t node_idx,
                       const char *member, size_t member_len,
                       uint32_t *midx_out) {
  const btree_node_t *node = &tree->nodes[node_idx];

  if (node->is_leaf) {
    return find_in_leaf(tree, node, member, member_len, midx_out);
  }

  for (uint8_t c = 0; c <= node->count; c++) {
    if (find_member(tree, node->children[c], member, member_len, midx_out) ==
        0) {
      return 0;
    }
  }
  return -1;
}

static void leaf_insert_at(btree_node_t *node, int pos, int64_t score,
                           uint32_t midx) {
  for (int i = (int)node->count; i > pos; i--) {
    node->keys[i] = node->keys[i - 1];
    node->values[i] = node->values[i - 1];
  }
  node->keys[pos] = score;
  node->values[pos] = midx;
  node->count++;
}

static int leaf_find_pos(const zedis_btree_t *tree, const btree_node_t *node,
                         int64_t score, uint32_t midx) {
  const btree_member_t *ins = &tree->members[midx];
  for (uint8_t i = 0; i < node->count; i++) {
    const btree_member_t *m = &tree->members[node->values[i]];
    int cmp = score_member_cmp(score, ins, node->keys[i], m->data, m->len);
    if (cmp <= 0) {
      return (int)i;
    }
  }
  return (int)node->count;
}

static uint32_t split_leaf(zedis_btree_t *tree, uint32_t leaf_idx,
                           int64_t *promoted) {
  btree_node_t *leaf = &tree->nodes[leaf_idx];
  uint32_t right_idx = alloc_node(tree);
  if (right_idx == UINT32_MAX) {
    return UINT32_MAX;
  }

  btree_node_t *right = &tree->nodes[right_idx];
  memset(right, 0, sizeof(*right));
  right->is_leaf = 1;

  int mid = ((int)leaf->count + 1) / 2;
  *promoted = leaf->keys[mid];

  right->count = (uint8_t)(leaf->count - (uint8_t)mid);
  leaf->count = (uint8_t)mid;

  for (int i = 0; i < (int)right->count; i++) {
    right->keys[i] = leaf->keys[mid + i];
    right->values[i] = leaf->values[mid + i];
  }

  return right_idx;
}

static uint32_t split_internal(zedis_btree_t *tree, uint32_t node_idx,
                               int64_t *promoted) {
  btree_node_t *node = &tree->nodes[node_idx];
  uint32_t right_idx = alloc_node(tree);
  if (right_idx == UINT32_MAX) {
    return UINT32_MAX;
  }

  btree_node_t *right = &tree->nodes[right_idx];
  memset(right, 0, sizeof(*right));

  int mid = BTREE_MAX_KEYS / 2;
  *promoted = node->keys[mid];

  right->count = (uint8_t)(node->count - (uint8_t)mid - 1);
  node->count = (uint8_t)mid;

  for (int i = 0; i < (int)right->count; i++) {
    right->keys[i] = node->keys[mid + 1 + i];
  }
  for (int i = 0; i <= (int)right->count; i++) {
    right->children[i] = node->children[mid + 1 + i];
  }

  return right_idx;
}

static int insert_into_leaf(zedis_btree_t *tree, uint32_t leaf_idx,
                            int64_t score, uint32_t midx) {
  btree_node_t *leaf = &tree->nodes[leaf_idx];
  int pos = leaf_find_pos(tree, leaf, score, midx);
  leaf_insert_at(leaf, pos, score, midx);
  (void)tree;
  return 0;
}

static int insert_nonfull(zedis_btree_t *tree, uint32_t node_idx, int64_t score,
                          uint32_t midx) {
  btree_node_t *node = &tree->nodes[node_idx];

  if (node->is_leaf) {
    return insert_into_leaf(tree, node_idx, score, midx);
  }

  int i = 0;
  while (i < (int)node->count && score > node->keys[i]) {
    i++;
  }

  uint32_t child = node->children[i];
  btree_node_t *child_node = &tree->nodes[child];

  if (child_node->count >= BTREE_MAX_KEYS) {
    int64_t promoted = 0;
    uint32_t sibling;
    if (child_node->is_leaf) {
      sibling = split_leaf(tree, child, &promoted);
    } else {
      sibling = split_internal(tree, child, &promoted);
    }
    if (sibling == UINT32_MAX) {
      return -1;
    }

    for (int j = (int)node->count; j > i; j--) {
      node->keys[j] = node->keys[j - 1];
      node->children[j + 1] = node->children[j];
    }
    node->keys[i] = promoted;
    node->children[i + 1] = sibling;
    node->count++;

    if (score >= promoted) {
      child = sibling;
    }
  }

  return insert_nonfull(tree, child, score, midx);
}

static int insert_entry(zedis_btree_t *tree, int64_t score, uint32_t midx) {
  if (tree->root == UINT32_MAX) {
    uint32_t root = alloc_node(tree);
    if (root == UINT32_MAX) {
      return -1;
    }
    tree->root = root;
    btree_node_t *node = &tree->nodes[root];
    node->is_leaf = 1;
    node->count = 1;
    node->keys[0] = score;
    node->values[0] = midx;
    return 0;
  }

  btree_node_t *root = &tree->nodes[tree->root];
  if (root->count < BTREE_MAX_KEYS) {
    return insert_nonfull(tree, tree->root, score, midx);
  }

  int64_t promoted = 0;
  uint32_t new_root = alloc_node(tree);
  uint32_t sibling;
  if (root->is_leaf) {
    sibling = split_leaf(tree, tree->root, &promoted);
  } else {
    sibling = split_internal(tree, tree->root, &promoted);
  }
  if (sibling == UINT32_MAX || new_root == UINT32_MAX) {
    return -1;
  }

  btree_node_t *nr = &tree->nodes[new_root];
  memset(nr, 0, sizeof(*nr));
  nr->count = 1;
  nr->keys[0] = promoted;
  nr->children[0] = tree->root;
  nr->children[1] = sibling;
  tree->root = new_root;

  if (score >= promoted) {
    return insert_nonfull(tree, sibling, score, midx);
  }
  return insert_nonfull(tree, tree->root, score, midx);
}

static int count_entries(const zedis_btree_t *tree, uint32_t node_idx) {
  const btree_node_t *node = &tree->nodes[node_idx];
  if (node->is_leaf) {
    return (int)node->count;
  }

  int total = 0;
  for (uint8_t i = 0; i <= node->count; i++) {
    total += count_entries(tree, node->children[i]);
  }
  return total;
}

static void collect_inorder(const zedis_btree_t *tree, uint32_t node_idx,
                            int64_t *idx, int64_t start, int64_t stop,
                            char *reply, size_t reply_cap, int *reply_len) {
  const btree_node_t *node = &tree->nodes[node_idx];

  if (node->is_leaf) {
    for (uint8_t i = 0; i < node->count; i++) {
      if (*idx >= start && *idx <= stop) {
        const btree_member_t *m = &tree->members[node->values[i]];
        int n = snprintf(reply + *reply_len, reply_cap - (size_t)*reply_len,
                         "$%zu\r\n%.*s\r\n", m->len, (int)m->len, m->data);
        if (n > 0 && (size_t)*reply_len + (size_t)n < reply_cap) {
          *reply_len += n;
        }
      }
      (*idx)++;
    }
    return;
  }

  for (uint8_t i = 0; i <= node->count; i++) {
    collect_inorder(tree, node->children[i], idx, start, stop, reply, reply_cap,
                    reply_len);
  }
}

int zedis_btree_init(zedis_btree_t *tree, zedis_arena_t *arena,
                     uint32_t max_members) {
  if (tree == NULL || arena == NULL || max_members == 0) {
    return -1;
  }

  if (max_members > ZEDIS_BTREE_MAX_ENTRIES) {
    max_members = ZEDIS_BTREE_MAX_ENTRIES;
  }

  memset(tree, 0, sizeof(*tree));
  tree->max_members = max_members;
  tree->max_nodes = max_members * 2 + 4;
  tree->root = UINT32_MAX;

  tree->nodes =
      zedis_arena_calloc(arena, tree->max_nodes, sizeof(btree_node_t), 64);
  tree->members =
      zedis_arena_calloc(arena, max_members, sizeof(btree_member_t), 64);
  if (tree->nodes == NULL || tree->members == NULL) {
    return -1;
  }

  return 0;
}

void zedis_btree_destroy(zedis_btree_t *tree) {
  if (tree == NULL) {
    return;
  }
  memset(tree, 0, sizeof(*tree));
}

static void rebuild_btree(zedis_btree_t *tree) {
  memset(tree->nodes, 0, tree->max_nodes * sizeof(btree_node_t));
  tree->root = UINT32_MAX;
  tree->free_node = 0;

  for (uint32_t i = 0; i < tree->member_count; i++) {
    insert_entry(tree, tree->members[i].score, i);
  }
}

int zedis_btree_add(zedis_btree_t *tree, int64_t score, const char *member,
                    size_t member_len) {
  if (member_len == 0 || member_len >= ZEDIS_BTREE_MAX_MEMBER) {
    return -1;
  }

  uint32_t existing = 0;
  if (tree->root != UINT32_MAX &&
      find_member(tree, tree->root, member, member_len, &existing) == 0) {
    tree->members[existing].score = score;
    rebuild_btree(tree);
    return 0;
  }

  uint32_t midx = alloc_member(tree, score, member, member_len);
  if (midx == UINT32_MAX) {
    return -1;
  }

  return insert_entry(tree, score, midx);
}

int zedis_btree_score(const zedis_btree_t *tree, const char *member,
                      size_t member_len, int64_t *score_out) {
  if (tree->root == UINT32_MAX) {
    return -1;
  }

  uint32_t midx = 0;
  if (find_member(tree, tree->root, member, member_len, &midx) != 0) {
    return -1;
  }

  if (score_out != NULL) {
    *score_out = tree->members[midx].score;
  }
  return 0;
}

int zedis_btree_range(const zedis_btree_t *tree, int64_t start_idx,
                      int64_t stop_idx, char *reply, size_t reply_cap,
                      int *reply_len) {
  if (tree->root == UINT32_MAX) {
    int n = snprintf(reply, reply_cap, "*0\r\n");
    if (n <= 0 || (size_t)n >= reply_cap) {
      return -1;
    }
    *reply_len = n;
    return 0;
  }

  int total = count_entries(tree, tree->root);
  if (start_idx < 0) {
    start_idx = 0;
  }
  if (stop_idx < 0 || stop_idx >= total) {
    stop_idx = total - 1;
  }
  if (start_idx > stop_idx || total == 0) {
    int n = snprintf(reply, reply_cap, "*0\r\n");
    if (n <= 0 || (size_t)n >= reply_cap) {
      return -1;
    }
    *reply_len = n;
    return 0;
  }

  int count = (int)(stop_idx - start_idx + 1);
  int n = snprintf(reply, reply_cap, "*%d\r\n", count);
  if (n <= 0 || (size_t)n >= reply_cap) {
    return -1;
  }
  *reply_len = n;

  int64_t idx = 0;
  collect_inorder(tree, tree->root, &idx, start_idx, stop_idx, reply, reply_cap,
                  reply_len);
  return 0;
}
