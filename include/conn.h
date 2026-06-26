#ifndef ZEDIS_CONN_H
#define ZEDIS_CONN_H

#include <stdbool.h>
#include <stddef.h>

#include "event_loop.h"
#include "resp.h"
#include "slab.h"

typedef struct zedis_server zedis_server_t;

typedef struct conn {
  int fd;
  zedis_server_t *server;
  event_loop_t *loop;

  resp_parser_t parser;

  char *write_buf;
  size_t write_len;
  size_t write_sent;

  char subscribe_channel[64];
  int subscribed;

  bool closed;

  struct conn *next;
  struct conn *prev;
} conn_t;

conn_t *conn_create(int fd, zedis_server_t *server, event_loop_t *loop,
                    zedis_slab_t *slab);

void conn_destroy(conn_t *conn);

void conn_on_readable(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata);
void conn_on_writable(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata);

int conn_queue_write(conn_t *conn, const char *data, size_t len);

#endif /* ZEDIS_CONN_H */