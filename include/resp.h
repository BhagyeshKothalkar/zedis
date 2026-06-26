#ifndef ZEDIS_RESP_H
#define ZEDIS_RESP_H

/*
 * resp.h — Stateful, streaming RESP parser
 *
 * Design goals
 * ------------
 *  1. Feed raw bytes incrementally via resp_parser_feed().
 *  2. Pull complete, parsed commands one at a time via resp_parser_next().
 *  3. Handle fragmented reads (partial messages) transparently.
 *  4. Handle pipelined reads (multiple complete + partial messages in one
 *     read(2) call) transparently.
 *  5. Zero-copy for bulk data: bulk pointers reference bytes inside the
 *     internal ring buffer, valid until the *next* call to resp_parser_next()
 *     or resp_parser_feed() that would overwrite them.  Callers that need to
 *     retain a value past that point must copy it.
 *
 * Internal buffer
 * ---------------
 * The parser owns a flat byte array [buf, buf+cap).  Two cursors split it:
 *
 *   [0 .. r_pos)   — already consumed / free space (after compaction)
 *   [r_pos .. w_pos) — buffered, not yet fully parsed
 *
 * On feed: bytes are appended at w_pos.
 * On next: the parser walks from r_pos, advancing it past each complete
 *          RESP value it decodes.
 *
 * Compaction (sliding the unread window back to index 0) happens in
 * resp_parser_feed() only when the free space at the front is large enough
 * to absorb the incoming bytes — so it is O(unread bytes), not O(buffer
 * size), and amortises to O(1) per byte across a session.
 */

#include "slab.h"

#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * RESP value types
 * ---------------------------------------------------------------------- */

typedef enum resp_type {
  RESP_TYPE_INCOMPLETE = 0,
  RESP_TYPE_SIMPLE,
  RESP_TYPE_ERROR,
  RESP_TYPE_INTEGER,
  RESP_TYPE_BULK,
  RESP_TYPE_NULL,
  RESP_TYPE_ARRAY
} resp_type_t;

/*
 * A node in a linked list used to represent RESP array elements.
 * Nodes are recycled through the parser's free-list to avoid repeated
 * slab allocation on every command.
 */
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

/*
 * A fully-parsed RESP value returned by resp_parser_next().
 *
 * Bulk/simple/error string data is zero-copy: the char* fields point
 * directly into the parser's internal buffer.  The data is valid until:
 *   - the next call to resp_parser_feed(), OR
 *   - the next call to resp_parser_next()
 * whichever comes first.  If you need the data to outlive those calls,
 * memcpy it yourself.
 *
 * Call resp_value_release() when done with the value to return node
 * memory to the free-list.
 */
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

/* -------------------------------------------------------------------------
 * Parser object
 * ---------------------------------------------------------------------- */

/*
 * Default internal buffer capacity.  Must be large enough for the biggest
 * single RESP message you expect to receive.  Operators can override via
 * resp_parser_init_ex().
 */
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

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/*
 * Initialise *parser with the default buffer size (RESP_PARSER_DEFAULT_BUF).
 * slab may be NULL; if non-NULL it is used for node allocation.
 * Returns 0 on success, -1 on allocation failure.
 */
int resp_parser_init(resp_parser_t *parser, struct zedis_slab *slab);

/*
 * Like resp_parser_init() but lets the caller specify the buffer capacity.
 */
int resp_parser_init_ex(resp_parser_t *parser, struct zedis_slab *slab,
                        size_t buf_cap);

/*
 * Release all memory owned by *parser.  After this call *parser is invalid
 * unless re-initialised.
 */
void resp_parser_destroy(resp_parser_t *parser);

/*
 * Reset parser to the initial (empty) state without releasing memory.
 * Any partially-buffered data is discarded.
 */
void resp_parser_reset(resp_parser_t *parser);

/* -------------------------------------------------------------------------
 * Feeding data
 * ---------------------------------------------------------------------- */

/*
 * Append [data, data+len) to the parser's internal buffer.
 *
 * Returns  0  on success.
 * Returns -1  if the buffer is full (caller must close the connection or
 *             increase the buffer capacity).
 *
 * After a successful feed, call resp_parser_next() in a loop until it
 * returns RESP_NEED_MORE.
 */
int resp_parser_feed(resp_parser_t *parser, const char *data, size_t len);

/*
 * How many bytes of free space remain in the internal buffer.
 * Useful for sizing a read(2) call: read at most resp_parser_space() bytes
 * directly into the buffer via resp_parser_write_ptr() / resp_parser_commit().
 */
size_t resp_parser_space(const resp_parser_t *parser);

/*
 * Zero-copy write path (optional, avoids a double-copy when reading from a
 * socket directly into the parser buffer):
 *
 *   char *dst = resp_parser_write_ptr(parser);
 *   ssize_t n = read(fd, dst, resp_parser_space(parser));
 *   if (n > 0) resp_parser_commit(parser, (size_t)n);
 */
char *resp_parser_write_ptr(resp_parser_t *parser);
void resp_parser_commit(resp_parser_t *parser, size_t n);

/* -------------------------------------------------------------------------
 * Consuming parsed values
 * ---------------------------------------------------------------------- */

/*
 * Try to parse one complete RESP value from the internal buffer.
 *
 *  RESP_OK        — *out is populated; caller owns the value and must call
 *                   resp_value_release() when done.
 *  RESP_NEED_MORE — not enough data yet; call resp_parser_feed() and retry.
 *  RESP_ERROR     — unrecoverable protocol error; close the connection.
 */
resp_parse_status_t resp_parser_next(resp_parser_t *parser, resp_value_t *out);

/*
 * Release resources associated with *value (returns nodes to the free-list).
 * Must be called for every value returned as RESP_OK.
 */
void resp_value_release(resp_parser_t *parser, resp_value_t *value);

/* -------------------------------------------------------------------------
 * Formatting helpers (write side — unchanged API)
 * ---------------------------------------------------------------------- */

int resp_format_simple(char *dst, size_t cap, const char *str);
int resp_format_bulk(char *dst, size_t cap, const char *data, size_t len);
int resp_format_error(char *dst, size_t cap, const char *msg);
int resp_format_integer(char *dst, size_t cap, int64_t value);
int resp_format_null(char *dst, size_t cap);

#endif /* ZEDIS_RESP_H */