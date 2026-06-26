#ifndef ZEDIS_RESP_H
#define ZEDIS_RESP_H

#include "slab.h"

#include <stddef.h>
#include <stdint.h>

typedef enum resp_type {
  RESP_TYPE_INCOMPLETE = 0,
  RESP_TYPE_SIMPLE,
  RESP_TYPE_ERROR,
  RESP_TYPE_INTEGER,
  RESP_TYPE_BULK,
  RESP_TYPE_NULL,
  RESP_TYPE_ARRAY
} resp_type_t;

typedef struct resp_node {
  struct resp_node *next;
  struct resp_node *prev;

  resp_type_t type;
  int64_t integer;
  const char
      *bulk; /* points into parser->buf; valid until next feed/next call */
  size_t bulk_len;

  /* Non-NULL only when this node holds a nested array. */
  struct resp_node *child_head;
  struct resp_node *child_tail;
  size_t child_count;
} resp_node_t;

typedef struct resp_value {
  resp_type_t type;
  int64_t integer;
  const char *bulk;
  size_t bulk_len;
  resp_node_t *array_head;
  resp_node_t *array_tail;
  size_t array_count;
} resp_value_t;

/* Return codes for resp_parser_next(). */
typedef enum resp_parse_status {
  RESP_NEED_MORE = 0, /* Buffer does not yet contain a complete message.  */
  RESP_OK,            /* A complete value was parsed into *out.            */
  RESP_ERROR          /* Protocol error; the connection should be closed.  */
} resp_parse_status_t;

#define RESP_PARSER_DEFAULT_BUF (64 * 1024) /* 64 KiB */

typedef struct resp_parser {
  /* Internal byte buffer */
  char *buf;
  size_t buf_cap; /* total capacity                    */
  size_t r_pos;   /* first unread byte                 */
  size_t w_pos;   /* one past the last written byte    */

  /* Free-list of resp_node_t objects recycled between commands. */
  resp_node_t *free_list;

  /* Slab allocator used for initial node allocation.  May be NULL,
   * in which case malloc() is used as a fallback. */
  struct zedis_slab *slab;
} resp_parser_t;

int resp_parser_init(resp_parser_t *parser, struct zedis_slab *slab);

int resp_parser_init_ex(resp_parser_t *parser, struct zedis_slab *slab,
                        size_t buf_cap);

void resp_parser_destroy(resp_parser_t *parser);

void resp_parser_reset(resp_parser_t *parser);

int resp_parser_feed(resp_parser_t *parser, const char *data, size_t len);

size_t resp_parser_space(const resp_parser_t *parser);

char *resp_parser_write_ptr(resp_parser_t *parser);
void resp_parser_commit(resp_parser_t *parser, size_t n);

resp_parse_status_t resp_parser_next(resp_parser_t *parser, resp_value_t *out);

void resp_value_release(resp_parser_t *parser, resp_value_t *value);

int resp_format_simple(char *dst, size_t cap, const char *str);
int resp_format_bulk(char *dst, size_t cap, const char *data, size_t len);
int resp_format_error(char *dst, size_t cap, const char *msg);
int resp_format_integer(char *dst, size_t cap, int64_t value);
int resp_format_null(char *dst, size_t cap);

#endif /* ZEDIS_RESP_H */