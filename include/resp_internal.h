#ifndef ZEDIS_RESP_INTERNAL_H
#define ZEDIS_RESP_INTERNAL_H

#include "resp.h"

/* Node freelist / slab helpers (resp_nodes.c) */
resp_node_t *resp__node_alloc(resp_parser_t *parser);
void resp__node_free(resp_parser_t *parser, resp_node_t *node);
void resp__node_list_release(resp_parser_t *parser, resp_node_t *head);
void resp__node_append(resp_node_t **head, resp_node_t **tail, size_t *count,
                       resp_node_t *node);

/* Buffer helper (resp_buf.c) */
void resp__compact(resp_parser_t *parser);

/* Scanning helpers (resp_scan.c) */
int resp__find_crlf(const resp_parser_t *parser, size_t from, size_t *pos);
int resp__parse_int(const char *buf, size_t from, size_t to, int64_t *out);
resp_parse_status_t resp__parse_inline(resp_parser_t *parser, size_t *pos,
                                       resp_value_t *out);

#endif /* ZEDIS_RESP_INTERNAL_H */