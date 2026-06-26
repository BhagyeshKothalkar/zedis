#ifndef ZEDIS_SERVER_H
#define ZEDIS_SERVER_H

#include <stdbool.h>

#include "append_log.h"
#include "arena.h"
#include "conn.h"
#include "event_loop.h"
#include "hash_table.h"
#include "key_registry.h"
#include "order_book.h"
#include "ring_buffer.h"
#include "slab.h"
#include "zedis.h"

struct zedis_server {
  zedis_config_t config;
  event_loop_t *loop;
  int listen_fd;
  bool running;

  zedis_arena_t arena;
  zedis_slab_t slab;
  zedis_hash_table_t kv;
  zedis_order_book_t book;
  zedis_ring_buffer_t ring;
  zedis_append_log_t aol;
  zedis_zset_registry_t zsets;
  zedis_list_registry_t lists;

  conn_t *conns_head;
  size_t conn_count;
};

void server_link_conn(zedis_server_t *server, conn_t *conn);
void server_unlink_conn(zedis_server_t *server, conn_t *conn);
void server_broadcast_channel(zedis_server_t *server, const char *channel,
                              const char *payload, size_t payload_len);
int server_handle_command(conn_t *conn, resp_value_t *cmd);
int server_dispatch_commands(conn_t *conn);
void server_on_accept(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata);

#endif /* ZEDIS_SERVER_H */