#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "affinity.h"

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
}

static int str_case_equal(const char *a, size_t a_len, const char *b) {
  if (strlen(b) != a_len) return 0;
  for (size_t i = 0; i < a_len; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
  }
  return 1;
}

static int parse_i64(const char *s, size_t len, int64_t *out) {
  if (len == 0) return -1;
  int negative = 0;
  size_t i = 0;
  if (s[0] == '-') {
    negative = 1;
    i = 1;
  }
  if (i >= len) return -1;
  int64_t value = 0;
  for (; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    value = value * 10 + (s[i] - '0');
  }
  *out = negative ? -value : value;
  return 0;
}

static int parse_int(const char *s, size_t len, int *out) {
  int64_t v = 0;
  if (parse_i64(s, len, &v) != 0) return -1;
  *out = (int)v;
  return 0;
}

static resp_node_t *array_nth(resp_value_t *cmd, size_t n) {
  resp_node_t *node = cmd->array_head;
  for (size_t i = 0; i < n && node != NULL; i++) {
    node = node->next;
  }
  return node;
}

void server_link_conn(zedis_server_t *server, conn_t *conn) {
  conn->next = server->conns_head;
  conn->prev = NULL;
  if (server->conns_head != NULL) {
    server->conns_head->prev = conn;
  }
  server->conns_head = conn;
  server->conn_count++;
}

void server_unlink_conn(zedis_server_t *server, conn_t *conn) {
  if (conn->prev != NULL) {
    conn->prev->next = conn->next;
  } else {
    server->conns_head = conn->next;
  }
  if (conn->next != NULL) {
    conn->next->prev = conn->prev;
  }
  if (server->conn_count > 0) {
    server->conn_count--;
  }
}

void server_broadcast_channel(zedis_server_t *server, const char *channel,
                              const char *payload, size_t payload_len) {
  char msg[ZEDIS_RING_MSG_MAX + 256];
  size_t ch_len = strlen(channel);

  int n = snprintf(msg, sizeof(msg),
                   "*3\r\n$7\r\nmessage\r\n$%zu\r\n%.*s\r\n$%zu\r\n", ch_len,
                   (int)ch_len, channel, payload_len);
  if (n <= 0 || (size_t)n >= sizeof(msg)) return;

  size_t used = (size_t)n;
  if (used + payload_len + 2 >= sizeof(msg)) return;

  memcpy(msg + used, payload, payload_len);
  used += payload_len;
  msg[used++] = '\r';
  msg[used++] = '\n';

  for (conn_t *c = server->conns_head; c != NULL; c = c->next) {
    if (c->subscribed &&
        str_case_equal(c->subscribe_channel, strlen(c->subscribe_channel),
                       channel)) {
      conn_queue_write(c, msg, used);
    }
  }
}


static int reply_error(conn_t *c, const char *msg) {
  char reply[512];
  int n = resp_format_error(reply, sizeof(reply), msg);
  if (n > 0) conn_queue_write(c, reply, (size_t)n);
  return 0;
}

static int reply_simple(conn_t *c, const char *msg) {
  char reply[512];
  int n = resp_format_simple(reply, sizeof(reply), msg);
  if (n > 0) conn_queue_write(c, reply, (size_t)n);
  return 0;
}

static int reply_integer(conn_t *c, int64_t v) {
  char reply[64];
  int n = resp_format_integer(reply, sizeof(reply), v);
  if (n > 0) conn_queue_write(c, reply, (size_t)n);
  return 0;
}

static int reply_bulk(conn_t *c, const char *s, size_t len) {
  char reply[4096];
  int n = resp_format_bulk(reply, sizeof(reply), s, len);
  if (n > 0) conn_queue_write(c, reply, (size_t)n);
  return 0;
}

static int reply_null(conn_t *c) {
  char reply[64];
  int n = resp_format_null(reply, sizeof(reply));
  if (n > 0) conn_queue_write(c, reply, (size_t)n);
  return 0;
}

/* --- Command Argument Extraction Helpers --- */

static int cmd_bulk(resp_value_t *cmd, size_t index, const char **data, size_t *len) {
  resp_node_t *node = array_nth(cmd, index);
  if (node == NULL || node->type != RESP_TYPE_BULK) return -1;
  *data = node->bulk;
  *len = node->bulk_len;
  return 0;
}

/* --- Command Handlers --- */

typedef int (*command_handler_fn)(conn_t *conn, resp_value_t *cmd);

static int ping_command(conn_t *conn, resp_value_t *cmd) {
  if (cmd->array_count == 1) {
    return reply_simple(conn, "PONG");
  }
  const char *msg;
  size_t len;
  if (cmd_bulk(cmd, 1, &msg, &len) == 0) {
    return reply_bulk(conn, msg, len);
  }
  return 0;
}

static int echo_command(conn_t *conn, resp_value_t *cmd) {
  const char *msg;
  size_t len;
  if (cmd_bulk(cmd, 1, &msg, &len) == 0) {
    return reply_bulk(conn, msg, len);
  }
  return reply_error(conn, "ERR wrong number of arguments for 'echo' command");
}

static int set_command(conn_t *conn, resp_value_t *cmd) {
  const char *key, *val;
  size_t klen, vlen;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0 || cmd_bulk(cmd, 2, &val, &vlen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'set' command");
  }
  if (zedis_ht_set(&conn->server->kv, key, klen, val, vlen) != 0) {
    return reply_error(conn, "ERR hash table full or value too large");
  }
  return reply_simple(conn, "OK");
}

static int get_command(conn_t *conn, resp_value_t *cmd) {
  const char *key;
  size_t klen;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'get' command");
  }
  char value[ZEDIS_HT_MAX_VALUE];
  size_t value_len = 0;
  if (zedis_ht_get(&conn->server->kv, key, klen, value, sizeof(value), &value_len) != 0) {
    return reply_null(conn);
  }
  return reply_bulk(conn, value, value_len);
}

static int bid_command(conn_t *conn, resp_value_t *cmd) {
  const char *price_str, *qty_str;
  size_t plen, qlen;
  if (cmd_bulk(cmd, 1, &price_str, &plen) != 0 || cmd_bulk(cmd, 2, &qty_str, &qlen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'bid' command");
  }
  int price = 0;
  int64_t qty = 0;
  if (parse_int(price_str, plen, &price) != 0 || parse_i64(qty_str, qlen, &qty) != 0) {
    return reply_error(conn, "ERR invalid bid arguments");
  }
  if (zedis_book_bid(&conn->server->book, price, qty) != 0) {
    return reply_error(conn, "ERR price out of range");
  }
  return reply_integer(conn, qty);
}

static int ask_command(conn_t *conn, resp_value_t *cmd) {
  const char *price_str, *qty_str;
  size_t plen, qlen;
  if (cmd_bulk(cmd, 1, &price_str, &plen) != 0 || cmd_bulk(cmd, 2, &qty_str, &qlen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'ask' command");
  }
  int price = 0;
  int64_t qty = 0;
  if (parse_int(price_str, plen, &price) != 0 || parse_i64(qty_str, qlen, &qty) != 0) {
    return reply_error(conn, "ERR invalid ask arguments");
  }
  if (zedis_book_ask(&conn->server->book, price, qty) != 0) {
    return reply_error(conn, "ERR price out of range");
  }
  return reply_integer(conn, qty);
}

static int book_command(conn_t *conn, resp_value_t *cmd) {
  const char *price_str;
  size_t plen;
  if (cmd_bulk(cmd, 1, &price_str, &plen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'book' command");
  }
  int price = 0;
  if (parse_int(price_str, plen, &price) != 0) {
    return reply_error(conn, "ERR invalid price");
  }
  int64_t bid = 0, ask = 0;
  if (zedis_book_level(&conn->server->book, price, &bid, &ask) != 0) {
    return reply_error(conn, "ERR price out of range");
  }
  char reply[128];
  int n = snprintf(reply, sizeof(reply), "*2\r\n:%" PRId64 "\r\n:%" PRId64 "\r\n", bid, ask);
  if (n > 0) conn_queue_write(conn, reply, (size_t)n);
  return 0;
}

static int del_command(conn_t *conn, resp_value_t *cmd) {
  const char *key;
  size_t klen;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'del' command");
  }
  int deleted = (zedis_ht_del(&conn->server->kv, key, klen) == 0) ? 1 : 0;
  return reply_integer(conn, deleted);
}

static int zadd_command(conn_t *conn, resp_value_t *cmd) {
  const char *key, *score_str, *member;
  size_t klen, slen, mlen;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0 || 
      cmd_bulk(cmd, 2, &score_str, &slen) != 0 || 
      cmd_bulk(cmd, 3, &member, &mlen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'zadd' command");
  }
  int64_t score = 0;
  if (parse_i64(score_str, slen, &score) != 0) {
    return reply_error(conn, "ERR value is not a valid float");
  }
  int64_t prev = 0;
  int existed = (zedis_zset_reg_score(&conn->server->zsets, key, klen, member, mlen, &prev) == 0);
  if (zedis_zset_reg_add(&conn->server->zsets, key, klen, score, member, mlen) != 0) {
    return reply_error(conn, "ERR zset full or member too large");
  }
  return reply_integer(conn, existed ? 0 : 1);
}

static int zscore_command(conn_t *conn, resp_value_t *cmd) {
  const char *key, *member;
  size_t klen, mlen;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0 || cmd_bulk(cmd, 2, &member, &mlen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'zscore' command");
  }
  int64_t score = 0;
  if (zedis_zset_reg_score(&conn->server->zsets, key, klen, member, mlen, &score) != 0) {
    return reply_null(conn);
  }
  char score_buf[64];
  int len = snprintf(score_buf, sizeof(score_buf), "%.17g", (double)score);
  if (len > 0) {
    return reply_bulk(conn, score_buf, (size_t)len);
  }
  return 0;
}

static int zrange_command(conn_t *conn, resp_value_t *cmd) {
  const char *key, *start_str, *stop_str;
  size_t klen, start_len, stop_len;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0 || 
      cmd_bulk(cmd, 2, &start_str, &start_len) != 0 || 
      cmd_bulk(cmd, 3, &stop_str, &stop_len) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'zrange' command");
  }
  int64_t start = 0, stop = 0;
  if (parse_i64(start_str, start_len, &start) != 0 || parse_i64(stop_str, stop_len, &stop) != 0) {
    return reply_error(conn, "ERR value is not an integer or out of range");
  }
  char zreply[8192];
  int zlen = 0;
  if (zedis_zset_reg_range(&conn->server->zsets, key, klen, start, stop, zreply, sizeof(zreply), &zlen) != 0) {
    return reply_error(conn, "ERR zrange failed");
  }
  conn_queue_write(conn, zreply, (size_t)zlen);
  return 0;
}

static int lpush_command(conn_t *conn, resp_value_t *cmd) {
  const char *key, *val;
  size_t klen, vlen;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0 || cmd_bulk(cmd, 2, &val, &vlen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'lpush' command");
  }
  if (zedis_list_reg_lpush(&conn->server->lists, &conn->server->aol, key, klen, val, vlen) != 0) {
    return reply_error(conn, "ERR list full or append log full");
  }
  uint32_t len = 0;
  zedis_list_reg_llen(&conn->server->lists, key, klen, &len);
  return reply_integer(conn, (int64_t)len);
}

static int lrange_command(conn_t *conn, resp_value_t *cmd) {
  const char *key, *start_str, *stop_str;
  size_t klen, start_len, stop_len;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0 || 
      cmd_bulk(cmd, 2, &start_str, &start_len) != 0 || 
      cmd_bulk(cmd, 3, &stop_str, &stop_len) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'lrange' command");
  }
  int64_t start = 0, stop = 0;
  if (parse_i64(start_str, start_len, &start) != 0 || parse_i64(stop_str, stop_len, &stop) != 0) {
    return reply_error(conn, "ERR value is not an integer or out of range");
  }
  char lreply[8192];
  int llen = 0;
  if (zedis_list_reg_lrange(&conn->server->lists, &conn->server->aol, key, klen, start, stop, lreply, sizeof(lreply), &llen) != 0) {
    return reply_error(conn, "ERR lrange failed");
  }
  conn_queue_write(conn, lreply, (size_t)llen);
  return 0;
}

static int llen_command(conn_t *conn, resp_value_t *cmd) {
  const char *key;
  size_t klen;
  if (cmd_bulk(cmd, 1, &key, &klen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'llen' command");
  }
  uint32_t len = 0;
  zedis_list_reg_llen(&conn->server->lists, key, klen, &len);
  return reply_integer(conn, (int64_t)len);
}

static int publish_command(conn_t *conn, resp_value_t *cmd) {
  const char *channel, *message;
  size_t clen, mlen;
  if (cmd_bulk(cmd, 1, &channel, &clen) != 0 || cmd_bulk(cmd, 2, &message, &mlen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'publish' command");
  }
  char ch[64];
  size_t ch_len = clen < sizeof(ch) - 1 ? clen : sizeof(ch) - 1;
  memcpy(ch, channel, ch_len);
  ch[ch_len] = '\0';

  if (zedis_ring_publish(&conn->server->ring, ch, message, mlen) != 0) {
    return reply_error(conn, "ERR ring buffer full");
  }

  size_t receivers = 0;
  for (conn_t *c = conn->server->conns_head; c != NULL; c = c->next) {
    if (c->subscribed && str_case_equal(c->subscribe_channel, strlen(c->subscribe_channel), ch)) {
      receivers++;
    }
  }

  server_broadcast_channel(conn->server, ch, message, mlen);
  return reply_integer(conn, (int64_t)receivers);
}

static int subscribe_command(conn_t *conn, resp_value_t *cmd) {
  const char *channel;
  size_t clen;
  if (cmd_bulk(cmd, 1, &channel, &clen) != 0) {
    return reply_error(conn, "ERR wrong number of arguments for 'subscribe' command");
  }

  size_t ch_len = clen < sizeof(conn->subscribe_channel) - 1 ? clen : sizeof(conn->subscribe_channel) - 1;
  memcpy(conn->subscribe_channel, channel, ch_len);
  conn->subscribe_channel[ch_len] = '\0';
  conn->subscribed = 1;

  char reply[512];
  int n = snprintf(reply, sizeof(reply),
                   "*3\r\n$9\r\nsubscribe\r\n$%zu\r\n%.*s\r\n:1\r\n", ch_len,
                   (int)ch_len, conn->subscribe_channel);
  if (n > 0) conn_queue_write(conn, reply, (size_t)n);
  return 0;
}

static int quit_command(conn_t *conn, resp_value_t *cmd) {
  (void)cmd;
  conn_destroy(conn);
  return 1;
}

/* --- Dispatcher --- */

typedef struct {
  const char *name;
  command_handler_fn fn;
} command_entry_t;

static const command_entry_t command_table[] = {
  { "PING",      ping_command },
  { "ECHO",      echo_command },
  { "SET",       set_command },
  { "GET",       get_command },
  { "BID",       bid_command },
  { "ASK",       ask_command },
  { "BOOK",      book_command },
  { "DEL",       del_command },
  { "ZADD",      zadd_command },
  { "ZSCORE",    zscore_command },
  { "ZRANGE",    zrange_command },
  { "LPUSH",     lpush_command },
  { "LRANGE",    lrange_command },
  { "LLEN",      llen_command },
  { "PUBLISH",   publish_command },
  { "SUBSCRIBE", subscribe_command },
  { "QUIT",      quit_command }
};

static const command_entry_t *find_command(const char *cmd, size_t len) {
  for (size_t i = 0; i < sizeof(command_table) / sizeof(command_table[0]); i++) {
    if (str_case_equal(cmd, len, command_table[i].name)) {
      return &command_table[i];
    }
  }
  return NULL;
}

int server_handle_command(conn_t *conn, resp_value_t *cmd) {
  if (cmd->type == RESP_TYPE_ARRAY && cmd->array_count == 0) {
    return 0;  // Silently ignore empty command line
  }

  if (cmd->type != RESP_TYPE_ARRAY) {
    return reply_error(conn, "ERR invalid command format");
  }

  const char *name;
  size_t len;
  if (cmd_bulk(cmd, 0, &name, &len) != 0) {
    return reply_error(conn, "ERR invalid command format");
  }

  const command_entry_t *entry = find_command(name, len);
  if (!entry) {
    char reply[512];
    int n = snprintf(reply, sizeof(reply), "-ERR unknown command '%.*s'\r\n", (int)len, name);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  return entry->fn(conn, cmd);
}

int server_dispatch_commands(conn_t *conn) {
  resp_value_t cmd;

  for (;;) {
    resp_parse_status_t status = resp_parser_next(&conn->parser, &cmd);

    if (status == RESP_NEED_MORE) {
      return 0;
    }

    if (status == RESP_ERROR) {
      char err[64];
      int n = resp_format_error(err, sizeof(err), "ERR protocol error");
      if (n > 0) conn_queue_write(conn, err, (size_t)n);
      conn_destroy(conn);
      return 1;
    }

    /* RESP_OK — dispatch the command. */
    int closed = server_handle_command(conn, &cmd);
    if (closed) {
      return 1;
    }
    if (conn->closed) {
      resp_value_release(&conn->parser, &cmd);
      conn_destroy(conn);
      return 1;
    }
    resp_value_release(&conn->parser, &cmd);
  }
}

void server_on_accept(event_loop_t *loop, int fd, uint32_t events,
                      void *userdata) {
  (void)events;

  zedis_server_t *server = userdata;

  if (server->conn_count >= server->config.max_connections) {
    return;
  }

  for (;;) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      return;
    }

    if (set_nonblocking(client_fd) != 0) {
      close(client_fd);
      continue;
    }

    conn_t *conn = conn_create(client_fd, server, loop, &server->slab);
    if (conn == NULL) {
      close(client_fd);
      continue;
    }

    if (event_loop_add(loop, client_fd, EPOLLIN | EPOLLET, conn_on_readable,
                       conn) != 0) {
      conn_destroy(conn);
    }
  }
}

static int create_listen_socket(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  int yes = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
    close(fd);
    return -1;
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, SOMAXCONN) != 0) {
    close(fd);
    return -1;
  }
  if (set_nonblocking(fd) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void server_defaults(zedis_config_t *cfg) {
  cfg->port = ZEDIS_DEFAULT_PORT;
  cfg->cpu_core = -1;
  cfg->arena_size = ZEDIS_DEFAULT_ARENA_SIZE;
  cfg->max_keys = ZEDIS_DEFAULT_MAX_KEYS;
  cfg->max_connections = ZEDIS_DEFAULT_MAX_CONNS;
  cfg->ring_capacity = ZEDIS_DEFAULT_RING_CAP;
  cfg->max_zsets = ZEDIS_DEFAULT_MAX_ZSETS;
  cfg->max_lists = ZEDIS_DEFAULT_MAX_LISTS;
  cfg->zset_members = ZEDIS_DEFAULT_ZSET_MEMBERS;
  cfg->aol_size = ZEDIS_DEFAULT_AOL_SIZE;
  cfg->aol_path = NULL;
  cfg->book_price_min = ZEDIS_DEFAULT_BOOK_MIN;
  cfg->book_price_max = ZEDIS_DEFAULT_BOOK_MAX;
  cfg->busy_poll = true;
}

zedis_server_t *zedis_create(const zedis_config_t *config) {
  zedis_config_t cfg;
  server_defaults(&cfg);
  if (config != NULL) cfg = *config;

  zedis_server_t *server = calloc(1, sizeof(*server));
  if (server == NULL) return NULL;

  server->config = cfg;

  if (zedis_arena_init(&server->arena, server->config.arena_size) != 0) {
    free(server);
    return NULL;
  }

  size_t slab_64 = server->config.max_connections * 32;
  size_t slab_256 = server->config.max_connections * 8;
  size_t slab_512 = server->config.max_connections * 2;
  size_t slab_4096 = server->config.max_connections * 2 + 64;

  if (zedis_slab_init(&server->slab, &server->arena, slab_64, slab_256,
                      slab_512, slab_4096) != 0) {
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  if (zedis_ht_init(&server->kv, &server->arena, server->config.max_keys) !=
      0) {
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  if (zedis_book_init(&server->book, &server->arena,
                      server->config.book_price_min,
                      server->config.book_price_max) != 0) {
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  if (zedis_ring_init(&server->ring, &server->arena,
                      server->config.ring_capacity) != 0) {
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  if (zedis_aol_init(&server->aol, server->config.aol_size,
                     server->config.aol_path) != 0) {
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  if (zedis_zset_reg_init(&server->zsets, &server->arena,
                          (uint32_t)server->config.max_zsets,
                          (uint32_t)server->config.zset_members) != 0) {
    zedis_aol_destroy(&server->aol);
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  if (zedis_list_reg_init(&server->lists, &server->arena,
                          (uint32_t)server->config.max_lists) != 0) {
    zedis_aol_destroy(&server->aol);
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  server->loop = event_loop_create(&server->arena, ZEDIS_MAX_FDS);
  if (server->loop == NULL) {
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  server->listen_fd = create_listen_socket(server->config.port);
  if (server->listen_fd < 0) {
    event_loop_destroy(server->loop);
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  if (event_loop_add(server->loop, server->listen_fd, EPOLLIN | EPOLLET,
                     server_on_accept, server) != 0) {
    close(server->listen_fd);
    event_loop_destroy(server->loop);
    zedis_arena_destroy(&server->arena);
    free(server);
    return NULL;
  }

  server->running = true;
  return server;
}

void zedis_destroy(zedis_server_t *server) {
  if (server == NULL) return;

  while (server->conns_head != NULL) {
    conn_destroy(server->conns_head);
  }

  if (server->listen_fd >= 0) {
    event_loop_del(server->loop, server->listen_fd);
    close(server->listen_fd);
  }

  event_loop_destroy(server->loop);
  zedis_slab_destroy(&server->slab);
  zedis_ht_destroy(&server->kv);
  zedis_book_destroy(&server->book);
  zedis_ring_destroy(&server->ring);
  zedis_aol_destroy(&server->aol);
  zedis_zset_reg_destroy(&server->zsets);
  zedis_list_reg_destroy(&server->lists);
  zedis_arena_destroy(&server->arena);
  free(server);
}

int zedis_run(zedis_server_t *server) {
  if (server == NULL) return -1;

  if (server->config.cpu_core >= 0) {
    if (zedis_pin_to_core(server->config.cpu_core) != 0) {
      fprintf(stderr, "zedis: warning: failed to pin to core %d\n",
              server->config.cpu_core);
    }
  }

  while (server->running) {
    if (event_loop_run_once(server->loop, !server->config.busy_poll) < 0) {
      return -1;
    }
  }

  return 0;
}