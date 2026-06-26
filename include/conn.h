#ifndef ZEDIS_CONN_H
#define ZEDIS_CONN_H

/*
 * conn.h — per-connection state
 *
 * Receive path (changed)
 * ----------------------
 * The old design used a flat read_buf[] + read_len pair.  After each read(2)
 * the code called resp_parse() on the whole buffer, then memmove()'d the
 * unconsumed tail back to offset 0.  That required two copies per message
 * (one into read_buf, one by the memmove) and could not amortise the slide
 * cost for large pipelined bursts.
 *
 * The new design removes read_buf entirely.  conn_t embeds a resp_parser_t
 * that owns its own receive buffer.  The readable callback reads directly
 * into that buffer via the zero-copy write-ptr API:
 *
 *   char   *dst = resp_parser_write_ptr(&conn->parser);
 *   ssize_t n   = read(conn->fd, dst, resp_parser_space(&conn->parser));
 *   resp_parser_commit(&conn->parser, (size_t)n);
 *   server_dispatch_commands(conn);      // drains all complete messages
 *
 * Benefits:
 *   • One fewer memcpy per read.
 *   • No memmove after every consumed command — the parser's sliding window
 *     compacts lazily and only when needed.
 *   • Pipelined commands are drained in a single loop without re-entering
 *     the event loop.
 *   • Partial messages are retained automatically; no manual read_len
 *     accounting in conn.c.
 *
 * Write path (unchanged)
 * ----------------------
 * write_buf / write_len / write_sent work exactly as before.
 */

#include "event_loop.h"
#include "resp.h"
#include "slab.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct zedis_server zedis_server_t;

typedef struct conn {
  int fd;
  zedis_server_t *server;
  event_loop_t *loop;

  /*
   * Receive buffer — owned by the embedded parser.
   * Do NOT add a separate read_buf here; all buffering goes through
   * conn->parser.
   */
  resp_parser_t parser;

  /* Write buffer — allocated from the slab in conn_create(). */
  char *write_buf;
  size_t write_len;
  size_t write_sent;

  /* Pub/sub state. */
  char subscribe_channel[64];
  int subscribed;

  bool closed;

  /* Intrusive doubly-linked list of all open connections. */
  struct conn *next;
  struct conn *prev;
} conn_t;

/*
 * Allocate and initialise a new connection for file-descriptor fd.
 * Returns NULL on allocation failure; the caller must close fd in that case.
 */
conn_t *conn_create(int fd, zedis_server_t *server, event_loop_t *loop,
                    zedis_slab_t *slab);

/*
 * Close fd, release all resources, and remove the connection from the
 * server's list.  *conn must not be accessed after this call.
 */
void conn_destroy(conn_t *conn);

/* epoll callbacks. */
void conn_on_readable(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata);
void conn_on_writable(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata);

/*
 * Append [data, data+len) to the write buffer and attempt a non-blocking
 * flush.  Returns 0 on success, -1 if the write buffer is full.
 */
int conn_queue_write(conn_t *conn, const char *data, size_t len);

#endif /* ZEDIS_CONN_H */