#include "conn.h"
#include "server.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>

/* =========================================================================
 * Write buffer helpers (unchanged from original)
 * ====================================================================== */

static int write_buf_append(conn_t *conn, const char *data, size_t len) {
  if (conn->closed) {
    return -1;
  }
  if (conn->write_len + len > ZEDIS_CONN_WRITE_SIZE) {
    return -1;
  }
  memcpy(conn->write_buf + conn->write_len, data, len);
  conn->write_len += len;
  return 0;
}

static void conn_enable_write(conn_t *conn) {
  event_loop_mod(conn->loop, conn->fd, EPOLLIN | EPOLLOUT | EPOLLET,
                 conn_on_readable, conn);
}

static void conn_disable_write(conn_t *conn) {
  event_loop_mod(conn->loop, conn->fd, EPOLLIN | EPOLLET, conn_on_readable,
                 conn);
}

/*
 * Drain as much of the write buffer as the socket will accept right now.
 *
 * Returns  0 — flushed completely (or partial flush registered with epoll).
 * Returns -1 — unrecoverable write error; caller should destroy the conn.
 */
static int conn_flush(conn_t *conn) {
  while (conn->write_sent < conn->write_len) {
    ssize_t n = write(conn->fd, conn->write_buf + conn->write_sent,
                      conn->write_len - conn->write_sent);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Socket buffer full — arm the write-ready event and return. */
        conn_enable_write(conn);
        // Shift unsent data to the beginning to reclaim space
        if (conn->write_sent > 0) {
          memmove(conn->write_buf, conn->write_buf + conn->write_sent,
                  conn->write_len - conn->write_sent);
          conn->write_len -= conn->write_sent;
          conn->write_sent = 0;
        }
        return 0;
      }
      conn->closed = 1;
      return -1;
    }
    conn->write_sent += (size_t)n;
  }

  /* Everything sent — reset the buffer and disarm write-ready. */
  conn->write_len = 0;
  conn->write_sent = 0;
  conn_disable_write(conn);
  return 0;
}

/* =========================================================================
 * Lifecycle
 * ====================================================================== */

conn_t *conn_create(int fd, zedis_server_t *server, event_loop_t *loop,
                    zedis_slab_t *slab) {
  conn_t *conn = zedis_slab_alloc(slab, ZEDIS_SLAB_512);
  if (conn == NULL) {
    return NULL;
  }
  memset(conn, 0, sizeof(*conn));

  conn->fd = fd;
  conn->server = server;
  conn->loop = loop;

  /*
   * Initialise the stateful RESP parser.  It allocates its own internal
   * receive buffer (RESP_PARSER_DEFAULT_BUF bytes via malloc); the slab
   * pointer is passed so node allocations can come from the arena-backed
   * slab instead of the system heap.
   *
   * NOTE: if you want the parser buffer itself to come from the slab too,
   * call resp_parser_init_ex() with a capacity that matches a slab class
   * and replace the malloc in resp_parser_init_ex with zedis_slab_alloc.
   * For now malloc is fine — there is at most one parser buffer per
   * connection and they are long-lived.
   */
  if (resp_parser_init(&conn->parser, slab) != 0) {
    zedis_slab_free(slab, conn, ZEDIS_SLAB_512);
    return NULL;
  }

  /* Allocate the write buffer from the slab. */
  conn->write_buf = zedis_slab_alloc(slab, ZEDIS_CONN_WRITE_SIZE);
  if (conn->write_buf == NULL) {
    resp_parser_destroy(&conn->parser);
    zedis_slab_free(slab, conn, ZEDIS_SLAB_512);
    return NULL;
  }

  server_link_conn(server, conn);
  return conn;
}

void conn_destroy(conn_t *conn) {
  if (conn == NULL) {
    return;
  }

  zedis_server_t *server = conn->server;
  zedis_slab_t *slab = (server != NULL) ? &server->slab : NULL;

  server_unlink_conn(server, conn);
  event_loop_del(conn->loop, conn->fd);
  close(conn->fd);

  resp_parser_destroy(&conn->parser);

  if (slab != NULL) {
    zedis_slab_free(slab, conn->write_buf, ZEDIS_CONN_WRITE_SIZE);
    zedis_slab_free(slab, conn, ZEDIS_SLAB_512);
  }
}

/* =========================================================================
 * Public write API
 * ====================================================================== */

int conn_queue_write(conn_t *conn, const char *data, size_t len) {
  if (conn->closed) {
    return -1;
  }
  if (write_buf_append(conn, data, len) != 0) {
    conn->closed = true;
    return -1;
  }
  if (conn_flush(conn) != 0) {
    conn->closed = true;
    return -1;
  }
  return 0;
}

/* =========================================================================
 * epoll callbacks
 * ====================================================================== */

/*
 * Called by the event loop whenever conn->fd becomes readable.
 *
 * Read loop
 * ---------
 * We use the parser's internal buffer as the destination for read(2)
 * directly, eliminating the intermediate copy into a separate read_buf:
 *
 *   dst = resp_parser_write_ptr()   — pointer to the free tail of the buffer
 *   n   = read(fd, dst, space)      — fill it straight from the kernel
 *   resp_parser_commit(n)           — advance w_pos by n
 *   server_dispatch_commands()      — drain every complete RESP message
 *
 * Edge-triggered (EPOLLET) semantics require us to read until EAGAIN before
 * returning; we do that in the inner for(;;) loop.
 *
 * If the parser buffer is completely full (space == 0) and we still have
 * not finished a single command, the client has sent a message larger than
 * RESP_PARSER_DEFAULT_BUF (64 KiB by default).  We treat that as a fatal
 * protocol error and close the connection.
 */
void conn_on_readable(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata) {
  (void)loop;
  (void)fd;
  (void)events;

  conn_t *conn = userdata;

  for (;;) {
    size_t space = resp_parser_space(&conn->parser);
    if (space == 0) {
      /*
       * The parser buffer is full but we haven't yielded a complete
       * command yet.  The client is either sending a pathologically
       * large message or is misbehaving.  Close the connection.
       */
      conn_destroy(conn);
      return;
    }

    char *dst = resp_parser_write_ptr(&conn->parser);
    ssize_t n = read(conn->fd, dst, space);

    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        /* Socket drained — stop reading until the next event. */
        break;
      }
      /* Unexpected error — close. */
      conn_destroy(conn);
      return;
    }

    if (n == 0) {
      /* Clean EOF from the client. */
      conn_destroy(conn);
      return;
    }

    resp_parser_commit(&conn->parser, (size_t)n);

    /*
     * Dispatch all complete commands that are now in the buffer.
     * server_dispatch_commands() returns 1 if the connection was closed
     * during dispatch (e.g. QUIT command or protocol error); in that
     * case we must not touch conn again.
     */
    if (server_dispatch_commands(conn) == 1) {
      return;
    }

    /*
     * Continue the read loop: under EPOLLET we must call read() again
     * to find out whether more data arrived while we were processing.
     * If the socket is drained, the next read() returns EAGAIN and we
     * break out naturally.
     */
  }
}

void conn_on_writable(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata) {
  (void)loop;
  (void)fd;
  (void)events;

  conn_t *conn = userdata;
  if (conn_flush(conn) != 0) {
    conn_destroy(conn);
  }
}