#include "resp.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "slab.h"

static resp_node_t *node_alloc(resp_parser_t *parser) {
  resp_node_t *node = parser->free_list;
  if (node != NULL) {
    parser->free_list = node->next;
    memset(node, 0, sizeof(*node));
    return node;
  }

  if (parser->slab != NULL) {
    resp_node_t *fresh = zedis_slab_alloc(parser->slab, sizeof(resp_node_t));
    if (fresh != NULL) {
      memset(fresh, 0, sizeof(*fresh));
      return fresh;
    }
  }

  resp_node_t *fresh = malloc(sizeof(*fresh));
  if (fresh != NULL) {
    memset(fresh, 0, sizeof(*fresh));
  }
  return fresh;
}

static void node_free(resp_parser_t *parser, resp_node_t *node) {
  node->next = parser->free_list;
  node->prev = NULL;
  parser->free_list = node;
}

static void node_list_release(resp_parser_t *parser, resp_node_t *head) {
  while (head != NULL) {
    resp_node_t *next = head->next;
    if (head->child_head != NULL) {
      node_list_release(parser, head->child_head);
      head->child_head = NULL;
    }
    node_free(parser, head);
    head = next;
  }
}

int resp_parser_init_ex(resp_parser_t *parser, struct zedis_slab *slab,
                        size_t buf_cap) {
  if (buf_cap == 0) {
    buf_cap = RESP_PARSER_DEFAULT_BUF;
  }

  char *buf = malloc(buf_cap);
  if (buf == NULL) {
    return -1;
  }

  memset(parser, 0, sizeof(*parser));
  parser->buf = buf;
  parser->buf_cap = buf_cap;
  parser->slab = slab;
  return 0;
}

int resp_parser_init(resp_parser_t *parser, struct zedis_slab *slab) {
  return resp_parser_init_ex(parser, slab, RESP_PARSER_DEFAULT_BUF);
}

void resp_parser_destroy(resp_parser_t *parser) {
  if (parser == NULL) {
    return;
  }

  if (parser->slab == NULL) {
    resp_node_t *n = parser->free_list;
    while (n != NULL) {
      resp_node_t *next = n->next;
      free(n);
      n = next;
    }
  }

  free(parser->buf);
  memset(parser, 0, sizeof(*parser));
}

void resp_parser_reset(resp_parser_t *parser) {
  parser->r_pos = 0;
  parser->w_pos = 0;
  /* Keep the free-list intact — nodes can be reused. */
}

static void compact(resp_parser_t *parser) {
  if (parser->r_pos == 0) {
    return;
  }
  size_t unread = parser->w_pos - parser->r_pos;
  if (unread > 0) {
    memmove(parser->buf, parser->buf + parser->r_pos, unread);
  }
  parser->w_pos = unread;
  parser->r_pos = 0;
}

size_t resp_parser_space(const resp_parser_t *parser) {
  return parser->buf_cap - parser->w_pos;
}

char *resp_parser_write_ptr(resp_parser_t *parser) {
  return parser->buf + parser->w_pos;
}

void resp_parser_commit(resp_parser_t *parser, size_t n) { parser->w_pos += n; }

int resp_parser_feed(resp_parser_t *parser, const char *data, size_t len) {
  if (len == 0) {
    return 0;
  }

  if (parser->w_pos + len <= parser->buf_cap) {
    memcpy(parser->buf + parser->w_pos, data, len);
    parser->w_pos += len;
    return 0;
  }

  size_t free_total = parser->buf_cap - (parser->w_pos - parser->r_pos);
  if (free_total < len) {
    return -1; /* buffer full */
  }

  compact(parser);
  memcpy(parser->buf + parser->w_pos, data, len);
  parser->w_pos += len;
  return 0;
}

static int find_crlf(const resp_parser_t *parser, size_t search_from,
                     size_t *crlf_pos) {
  const char *buf = parser->buf;
  size_t w = parser->w_pos;

  for (size_t i = search_from; i + 1 < w; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n') {
      *crlf_pos = i;
      return 0;
    }
  }
  return -1;
}

static int parse_int_range(const char *buf, size_t from, size_t to,
                           int64_t *out) {
  if (from >= to) {
    return -1;
  }

  int negative = 0;
  size_t i = from;

  if (buf[i] == '-') {
    negative = 1;
    i++;
  }

  if (i >= to || !isdigit((unsigned char)buf[i])) {
    return -1;
  }

  int64_t value = 0;
  for (; i < to; i++) {
    if (!isdigit((unsigned char)buf[i])) {
      return -1;
    }
    value = value * 10 + (buf[i] - '0');
  }

  *out = negative ? -value : value;
  return 0;
}

static void node_append(resp_node_t **head, resp_node_t **tail, size_t *count,
                        resp_node_t *node) {
  node->next = NULL;
  node->prev = *tail;

  if (*tail != NULL) {
    (*tail)->next = node;
  } else {
    *head = node;
  }

  *tail = node;
  (*count)++;
}

/* Forward declaration. */
static resp_parse_status_t parse_value(resp_parser_t *parser, size_t *pos,
                                       resp_value_t *out);

static resp_parse_status_t parse_inline(resp_parser_t *parser, size_t *pos,
                                        resp_value_t *out) {
  const char *buf = parser->buf;
  size_t w = parser->w_pos;
  size_t start = *pos;

  size_t lf_pos = 0;
  int found = 0;

  for (size_t i = start; i < w; i++) {
    if (buf[i] == '\n') {
      lf_pos = i;
      found = 1;
      break;
    }
  }

  if (!found) {
    return RESP_NEED_MORE;
  }
  size_t line_end =
      (lf_pos > start && buf[lf_pos - 1] == '\r') ? lf_pos - 1 : lf_pos;

  /* Position for the next command starts after the \n. */
  size_t next_pos = lf_pos + 1;

  resp_node_t *head = NULL;
  resp_node_t *tail = NULL;
  size_t child_count = 0;
  size_t i = start;

  while (i < line_end) {
    /* Skip inter-token whitespace. */
    while (i < line_end && (buf[i] == ' ' || buf[i] == '\t')) {
      i++;
    }
    if (i >= line_end) {
      break;
    }

    /* Find end of token. */
    size_t tok_start = i;
    while (i < line_end && buf[i] != ' ' && buf[i] != '\t') {
      i++;
    }

    resp_node_t *node = node_alloc(parser);
    if (node == NULL) {
      node_list_release(parser, head);
      return RESP_ERROR;
    }

    node->type = RESP_TYPE_BULK;
    node->bulk = (char *)(buf + tok_start); /* zero-copy into parser->buf */
    node->bulk_len = i - tok_start;

    node_append(&head, &tail, &child_count, node);
  }

  out->type = RESP_TYPE_ARRAY;
  out->array_head = head;
  out->array_tail = tail;
  out->array_count = child_count;
  *pos = next_pos;
  return RESP_OK;
}

static resp_parse_status_t parse_array(resp_parser_t *parser, size_t *pos,
                                       int64_t count, resp_value_t *out) {
  resp_node_t *head = NULL;
  resp_node_t *tail = NULL;
  size_t child_count = 0;

  for (int64_t i = 0; i < count; i++) {
    resp_node_t *child = node_alloc(parser);
    if (child == NULL) {
      node_list_release(parser, head);
      return RESP_ERROR;
    }

    resp_value_t nested;
    memset(&nested, 0, sizeof(nested));

    resp_parse_status_t status = parse_value(parser, pos, &nested);
    if (status != RESP_OK) {
      node_free(parser, child);
      node_list_release(parser, head);
      return status;
    }

    child->type = nested.type;
    child->integer = nested.integer;
    child->bulk = nested.bulk;
    child->bulk_len = nested.bulk_len;

    if (nested.type == RESP_TYPE_ARRAY) {
      child->child_head = nested.array_head;
      child->child_tail = nested.array_tail;
      child->child_count = nested.array_count;
    }

    node_append(&head, &tail, &child_count, child);
  }

  out->type = RESP_TYPE_ARRAY;
  out->array_head = head;
  out->array_tail = tail;
  out->array_count = child_count;
  return RESP_OK;
}

static resp_parse_status_t parse_value(resp_parser_t *parser, size_t *pos,
                                       resp_value_t *out) {
  const char *buf = parser->buf;
  size_t w = parser->w_pos;

  if (*pos >= w) {
    return RESP_NEED_MORE;
  }

  char prefix = buf[*pos];
  size_t line_start = *pos + 1;

  switch (prefix) {
    case '+':
    case '-':
    case ':':
    case '$':
    case '*':
      break;
    default:
      return parse_inline(parser, pos, out);
  }

  size_t crlf_pos = 0;
  if (find_crlf(parser, line_start, &crlf_pos) != 0) {
    return RESP_NEED_MORE;
  }

  switch (prefix) {
    /* +<string>\r\n */
    case '+':
      out->type = RESP_TYPE_SIMPLE;
      out->bulk = (char *)(buf + line_start);
      out->bulk_len = crlf_pos - line_start;
      *pos = crlf_pos + 2;
      return RESP_OK;

    /* -<message>\r\n */
    case '-':
      out->type = RESP_TYPE_ERROR;
      out->bulk = (char *)(buf + line_start);
      out->bulk_len = crlf_pos - line_start;
      *pos = crlf_pos + 2;
      return RESP_OK;

    /* :<integer>\r\n */
    case ':': {
      int64_t value = 0;
      if (parse_int_range(buf, line_start, crlf_pos, &value) != 0) {
        return RESP_ERROR;
      }
      out->type = RESP_TYPE_INTEGER;
      out->integer = value;
      *pos = crlf_pos + 2;
      return RESP_OK;
    }

    /* $<len>\r\n<data>\r\n */
    case '$': {
      int64_t bulk_len = 0;
      if (parse_int_range(buf, line_start, crlf_pos, &bulk_len) != 0) {
        return RESP_ERROR;
      }

      /* Null bulk string: $-1\r\n */
      if (bulk_len < 0) {
        out->type = RESP_TYPE_NULL;
        *pos = crlf_pos + 2;
        return RESP_OK;
      }

      size_t data_start = crlf_pos + 2;
      size_t data_end = data_start + (size_t)bulk_len;

      /* Need data_end + 2 bytes (\r\n) to be available. */
      if (data_end + 2 > w) {
        return RESP_NEED_MORE;
      }

      if (buf[data_end] != '\r' || buf[data_end + 1] != '\n') {
        return RESP_ERROR;
      }

      out->type = RESP_TYPE_BULK;
      out->bulk = (char *)(buf + data_start);
      out->bulk_len = (size_t)bulk_len;
      *pos = data_end + 2;
      return RESP_OK;
    }

    /* *<count>\r\n<element> ... */
    case '*': {
      int64_t count = 0;
      if (parse_int_range(buf, line_start, crlf_pos, &count) != 0) {
        return RESP_ERROR;
      }

      /* Null array: *-1\r\n */
      if (count < 0) {
        out->type = RESP_TYPE_NULL;
        *pos = crlf_pos + 2;
        return RESP_OK;
      }

      size_t array_start = crlf_pos + 2;
      resp_parse_status_t st = parse_array(parser, &array_start, count, out);
      if (st == RESP_OK) {
        *pos = array_start;
      }
      return st;
    }

    default:
      /* Unreachable — all non-RESP prefixes are caught above. */
      return RESP_ERROR;
  }
}

resp_parse_status_t resp_parser_next(resp_parser_t *parser, resp_value_t *out) {
  memset(out, 0, sizeof(*out));

  if (parser->r_pos > parser->buf_cap / 2) {
    compact(parser);
  }

  size_t pos = parser->r_pos;

  resp_parse_status_t status = parse_value(parser, &pos, out);

  if (status == RESP_OK) {
    parser->r_pos = pos;
  }

  return status;
}

void resp_value_release(resp_parser_t *parser, resp_value_t *value) {
  if (value == NULL) {
    return;
  }
  if (value->type == RESP_TYPE_ARRAY && value->array_head != NULL) {
    node_list_release(parser, value->array_head);
    value->array_head = NULL;
    value->array_tail = NULL;
    value->array_count = 0;
  }
  memset(value, 0, sizeof(*value));
}

static int append_str(char *dst, size_t cap, size_t *used, const char *s,
                      size_t len) {
  if (*used + len >= cap) {
    return -1;
  }
  memcpy(dst + *used, s, len);
  *used += len;
  return 0;
}

int resp_format_simple(char *dst, size_t cap, const char *str) {
  size_t used = 0;
  size_t str_len = strlen(str);

  if (append_str(dst, cap, &used, "+", 1) != 0) return -1;
  if (append_str(dst, cap, &used, str, str_len) != 0) return -1;
  if (append_str(dst, cap, &used, "\r\n", 2) != 0) return -1;
  dst[used] = '\0';
  return (int)used;
}

int resp_format_error(char *dst, size_t cap, const char *msg) {
  size_t used = 0;
  size_t msg_len = strlen(msg);

  if (append_str(dst, cap, &used, "-", 1) != 0) return -1;
  if (append_str(dst, cap, &used, msg, msg_len) != 0) return -1;
  if (append_str(dst, cap, &used, "\r\n", 2) != 0) return -1;
  dst[used] = '\0';
  return (int)used;
}

int resp_format_integer(char *dst, size_t cap, int64_t value) {
  int n = snprintf(dst, cap, ":%" PRId64 "\r\n", value);
  return (n > 0 && (size_t)n < cap) ? n : -1;
}

int resp_format_bulk(char *dst, size_t cap, const char *data, size_t len) {
  int n = snprintf(dst, cap, "$%zu\r\n", len);
  if (n <= 0 || (size_t)n >= cap) return -1;

  size_t used = (size_t)n;
  if (used + len + 3 > cap) return -1;

  memcpy(dst + used, data, len);
  used += len;
  memcpy(dst + used, "\r\n", 2);
  used += 2;
  dst[used] = '\0';
  return (int)used;
}

int resp_format_null(char *dst, size_t cap) {
  static const char null_bulk[] = "$-1\r\n";
  size_t len = sizeof(null_bulk) - 1;
  if (len >= cap) return -1;
  memcpy(dst, null_bulk, len);
  dst[len] = '\0';
  return (int)len;
}