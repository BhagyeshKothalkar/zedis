#include <stdlib.h>
#include <string.h>

#include "resp.h"
#include "resp_internal.h"

resp_node_t *resp__node_alloc(resp_parser_t *parser) {
  resp_node_t *node = parser->free_list;
  if (node != NULL) {
    parser->free_list = node->next;
    memset(node, 0, sizeof(*node));
    return node;
  }
  if (parser->slab != NULL) {
    node = zedis_slab_alloc(parser->slab, sizeof(*node));
    if (node != NULL) {
      memset(node, 0, sizeof(*node));
      return node;
    }
  }
  node = malloc(sizeof(*node));
  if (node != NULL) memset(node, 0, sizeof(*node));
  return node;
}
void resp__node_free(resp_parser_t *parser, resp_node_t *node) {
  node->next = parser->free_list;
  node->prev = NULL;
  parser->free_list = node;
}
void resp__node_list_release(resp_parser_t *parser, resp_node_t *head) {
  while (head != NULL) {
    resp_node_t *next = head->next;
    if (head->child_head != NULL) {
      resp__node_list_release(parser, head->child_head);
      head->child_head = NULL;
    }
    resp__node_free(parser, head);
    head = next;
  }
}
void resp__node_append(resp_node_t **head, resp_node_t **tail, size_t *count,
                       resp_node_t *node) {
  node->next = NULL;
  node->prev = *tail;
  if (*tail != NULL)
    (*tail)->next = node;
  else
    *head = node;
  *tail = node;
  (*count)++;
}