#include "conn.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "server.h"

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

  conn->write_len = 0;
  conn->write_sent = 0;
  conn_disable_write(conn);
  return 0;
}

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

  if (resp_parser_init(&conn->parser, slab) != 0) {
    zedis_slab_free(slab, conn, ZEDIS_SLAB_512);
    return NULL;
  }

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

void conn_on_readable(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata) {
  (void)loop;
  (void)fd;
  (void)events;

  conn_t *conn = userdata;

  for (;;) {
    size_t space = resp_parser_space(&conn->parser);
    if (space == 0) {
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

    if (server_dispatch_commands(conn) == 1) {
      return;
    }
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