#include <string.h>

#include "resp.h"
#include "resp_internal.h"

static resp_parse_status_t parse_value(resp_parser_t *p, size_t *pos,
                                       resp_value_t *out);
/* *<count>\r\n<element>... — parses `count` nested values recursively. */
static resp_parse_status_t parse_array(resp_parser_t *p, size_t *pos,
                                       int64_t count, resp_value_t *out) {
  resp_node_t *head = NULL, *tail = NULL;
  size_t n = 0;
  for (int64_t i = 0; i < count; i++) {
    resp_node_t *child = resp__node_alloc(p);
    if (child == NULL) {
      resp__node_list_release(p, head);
      return RESP_ERROR;
    }
    resp_value_t nested;
    memset(&nested, 0, sizeof(nested));
    resp_parse_status_t status = parse_value(p, pos, &nested);
    if (status != RESP_OK) {
      resp__node_free(p, child);
      resp__node_list_release(p, head);
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
    resp__node_append(&head, &tail, &n, child);
  }
  out->type = RESP_TYPE_ARRAY;
  out->array_head = head;
  out->array_tail = tail;
  out->array_count = n;
  return RESP_OK;
}

static resp_parse_status_t parse_value(resp_parser_t *p, size_t *pos,
                                       resp_value_t *out) {
  const char *buf = p->buf;
  size_t w = p->w_pos;
  if (*pos >= w) return RESP_NEED_MORE;
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
      return resp__parse_inline(p, pos, out);
  }
  size_t crlf = 0;
  if (resp__find_crlf(p, line_start, &crlf) != 0) return RESP_NEED_MORE;
  if (prefix == '+' || prefix == '-') {
    out->type = (prefix == '+') ? RESP_TYPE_SIMPLE : RESP_TYPE_ERROR;
    out->bulk = buf + line_start;
    out->bulk_len = crlf - line_start;
    *pos = crlf + 2;
    return RESP_OK;
  }
  if (prefix == ':') {
    int64_t value;
    if (resp__parse_int(buf, line_start, crlf, &value) != 0) return RESP_ERROR;
    out->type = RESP_TYPE_INTEGER;
    out->integer = value;
    *pos = crlf + 2;
    return RESP_OK;
  }
  if (prefix == '$') {
    int64_t bulk_len;
    if (resp__parse_int(buf, line_start, crlf, &bulk_len) != 0)
      return RESP_ERROR;
    if (bulk_len < 0) { /* $-1\r\n */
      out->type = RESP_TYPE_NULL;
      *pos = crlf + 2;
      return RESP_OK;
    }
    size_t data_start = crlf + 2, data_end = data_start + (size_t)bulk_len;
    if (data_end + 2 > w) return RESP_NEED_MORE;
    if (buf[data_end] != '\r' || buf[data_end + 1] != '\n') return RESP_ERROR;
    out->type = RESP_TYPE_BULK;
    out->bulk = buf + data_start;
    out->bulk_len = (size_t)bulk_len;
    *pos = data_end + 2;
    return RESP_OK;
  }
  /* '*' */
  int64_t count;
  if (resp__parse_int(buf, line_start, crlf, &count) != 0) return RESP_ERROR;
  if (count < 0) { /* *-1\r\n */
    out->type = RESP_TYPE_NULL;
    *pos = crlf + 2;
    return RESP_OK;
  }
  size_t array_start = crlf + 2;
  resp_parse_status_t st = parse_array(p, &array_start, count, out);
  if (st == RESP_OK) *pos = array_start;
  return st;
}

resp_parse_status_t resp_parser_next(resp_parser_t *parser, resp_value_t *out) {
  memset(out, 0, sizeof(*out));
  if (parser->r_pos > parser->buf_cap / 2) resp__compact(parser);
  size_t pos = parser->r_pos;
  resp_parse_status_t status = parse_value(parser, &pos, out);
  if (status == RESP_OK) parser->r_pos = pos;
  return status;
}

void resp_value_release(resp_parser_t *parser, resp_value_t *value) {
  if (value == NULL) return;
  if (value->type == RESP_TYPE_ARRAY && value->array_head != NULL) {
    resp__node_list_release(parser, value->array_head);
    value->array_head = NULL;
    value->array_tail = NULL;
    value->array_count = 0;
  }
  memset(value, 0, sizeof(*value));
}