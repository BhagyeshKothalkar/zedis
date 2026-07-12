#include <ctype.h>

#include "resp.h"
#include "resp_internal.h"

int resp__find_crlf(const resp_parser_t *p, size_t from, size_t *pos) {
  const char *buf = p->buf;
  size_t w = p->w_pos;
  for (size_t i = from; i + 1 < w; i++) {
    if (buf[i] == '\r' && buf[i + 1] == '\n') {
      *pos = i;
      return 0;
    }
  }
  return -1;
}
int resp__parse_int(const char *buf, size_t from, size_t to, int64_t *out) {
  if (from >= to) return -1;
  int neg = 0;
  size_t i = from;
  if (buf[i] == '-') {
    neg = 1;
    i++;
  }
  if (i >= to || !isdigit((unsigned char)buf[i])) return -1;
  int64_t value = 0;
  for (; i < to; i++) {
    if (!isdigit((unsigned char)buf[i])) return -1;
    value = value * 10 + (buf[i] - '0');
  }
  *out = neg ? -value : value;
  return 0;
}
/* A line with no RESP type prefix: whitespace-separated tokens up to \n. */
resp_parse_status_t resp__parse_inline(resp_parser_t *p, size_t *pos,
                                       resp_value_t *out) {
  const char *buf = p->buf;
  size_t w = p->w_pos, start = *pos, lf = 0;
  int found = 0;
  for (size_t i = start; i < w; i++) {
    if (buf[i] == '\n') {
      lf = i;
      found = 1;
      break;
    }
  }
  if (!found) return RESP_NEED_MORE;
  size_t line_end = (lf > start && buf[lf - 1] == '\r') ? lf - 1 : lf;
  resp_node_t *head = NULL, *tail = NULL;
  size_t count = 0, i = start;
  while (i < line_end) {
    while (i < line_end && (buf[i] == ' ' || buf[i] == '\t')) i++;
    if (i >= line_end) break;
    size_t tok = i;
    while (i < line_end && buf[i] != ' ' && buf[i] != '\t') i++;

    resp_node_t *node = resp__node_alloc(p);
    if (node == NULL) {
      resp__node_list_release(p, head);
      return RESP_ERROR;
    }
    node->type = RESP_TYPE_BULK;
    node->bulk = buf + tok; /* zero-copy into parser->buf */
    node->bulk_len = i - tok;
    resp__node_append(&head, &tail, &count, node);
  }

  out->type = RESP_TYPE_ARRAY;
  out->array_head = head;
  out->array_tail = tail;
  out->array_count = count;
  *pos = lf + 1;
  return RESP_OK;
}