#include <stdlib.h>
#include <string.h>

#include "resp.h"
#include "resp_internal.h"

int resp_parser_init_ex(resp_parser_t *parser, struct zedis_slab *slab,
                        size_t buf_cap) {
  if (buf_cap == 0) buf_cap = RESP_PARSER_DEFAULT_BUF;
  char *buf = malloc(buf_cap);
  if (buf == NULL) return -1;
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
  if (parser == NULL) return;
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
  /* Free-list stays intact — nodes are reused across commands. */
}
/* Shift unread bytes down to offset 0, reclaiming space at the tail. */
void resp__compact(resp_parser_t *parser) {
  if (parser->r_pos == 0) return;
  size_t unread = parser->w_pos - parser->r_pos;
  if (unread > 0) memmove(parser->buf, parser->buf + parser->r_pos, unread);
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
  if (len == 0) return 0;
  if (parser->w_pos + len <= parser->buf_cap) {
    memcpy(parser->buf + parser->w_pos, data, len);
    parser->w_pos += len;
    return 0;
  }
  size_t free_total = parser->buf_cap - (parser->w_pos - parser->r_pos);
  if (free_total < len) return -1; /* buffer full */
  resp__compact(parser);
  memcpy(parser->buf + parser->w_pos, data, len);
  parser->w_pos += len;
  return 0;
}