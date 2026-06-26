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

int server_handle_command(conn_t *conn, resp_value_t *cmd) {
  char reply[4096];
  zedis_server_t *server = conn->server;

  if (cmd->type == RESP_TYPE_ARRAY && cmd->array_count == 0) {
    return 0;  // Silently ignore empty command line
  }

  if (cmd->type != RESP_TYPE_ARRAY) {
    int n =
        resp_format_error(reply, sizeof(reply), "ERR invalid command format");
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  resp_node_t *command = array_nth(cmd, 0);
  if (command == NULL || command->type != RESP_TYPE_BULK) {
    int n =
        resp_format_error(reply, sizeof(reply), "ERR invalid command format");
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "PING")) {
    if (cmd->array_count == 1) {
      int n = resp_format_simple(reply, sizeof(reply), "PONG");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    resp_node_t *message = array_nth(cmd, 1);
    if (message != NULL && message->type == RESP_TYPE_BULK) {
      int n = resp_format_bulk(reply, sizeof(reply), message->bulk,
                               message->bulk_len);
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    }
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "ECHO")) {
    resp_node_t *message = array_nth(cmd, 1);
    if (message != NULL && message->type == RESP_TYPE_BULK) {
      int n = resp_format_bulk(reply, sizeof(reply), message->bulk,
                               message->bulk_len);
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int n =
        resp_format_error(reply, sizeof(reply),
                          "ERR wrong number of arguments for 'echo' command");
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "SET")) {
    resp_node_t *key = array_nth(cmd, 1);
    resp_node_t *val = array_nth(cmd, 2);
    if (key == NULL || val == NULL || key->type != RESP_TYPE_BULK ||
        val->type != RESP_TYPE_BULK) {
      int n =
          resp_format_error(reply, sizeof(reply),
                            "ERR wrong number of arguments for 'set' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    if (zedis_ht_set(&server->kv, key->bulk, key->bulk_len, val->bulk,
                     val->bulk_len) != 0) {
      int n = resp_format_error(reply, sizeof(reply),
                                "ERR hash table full or value too large");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int n = resp_format_simple(reply, sizeof(reply), "OK");
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "GET")) {
    resp_node_t *key = array_nth(cmd, 1);
    if (key == NULL || key->type != RESP_TYPE_BULK) {
      int n =
          resp_format_error(reply, sizeof(reply),
                            "ERR wrong number of arguments for 'get' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    char value[ZEDIS_HT_MAX_VALUE];
    size_t value_len = 0;
    if (zedis_ht_get(&server->kv, key->bulk, key->bulk_len, value,
                     sizeof(value), &value_len) != 0) {
      int n = resp_format_null(reply, sizeof(reply));
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int n = resp_format_bulk(reply, sizeof(reply), value, value_len);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "BID")) {
    resp_node_t *price_arg = array_nth(cmd, 1);
    resp_node_t *qty_arg = array_nth(cmd, 2);
    if (price_arg == NULL || qty_arg == NULL ||
        price_arg->type != RESP_TYPE_BULK || qty_arg->type != RESP_TYPE_BULK) {
      int n =
          resp_format_error(reply, sizeof(reply),
                            "ERR wrong number of arguments for 'bid' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int price = 0;
    int64_t qty = 0;
    if (parse_int(price_arg->bulk, price_arg->bulk_len, &price) != 0 ||
        parse_i64(qty_arg->bulk, qty_arg->bulk_len, &qty) != 0) {
      int n =
          resp_format_error(reply, sizeof(reply), "ERR invalid bid arguments");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    if (zedis_book_bid(&server->book, price, qty) != 0) {
      int n = resp_format_error(reply, sizeof(reply), "ERR price out of range");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int n = resp_format_integer(reply, sizeof(reply), qty);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "ASK")) {
    resp_node_t *price_arg = array_nth(cmd, 1);
    resp_node_t *qty_arg = array_nth(cmd, 2);
    if (price_arg == NULL || qty_arg == NULL ||
        price_arg->type != RESP_TYPE_BULK || qty_arg->type != RESP_TYPE_BULK) {
      int n =
          resp_format_error(reply, sizeof(reply),
                            "ERR wrong number of arguments for 'ask' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int price = 0;
    int64_t qty = 0;
    if (parse_int(price_arg->bulk, price_arg->bulk_len, &price) != 0 ||
        parse_i64(qty_arg->bulk, qty_arg->bulk_len, &qty) != 0) {
      int n =
          resp_format_error(reply, sizeof(reply), "ERR invalid ask arguments");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    if (zedis_book_ask(&server->book, price, qty) != 0) {
      int n = resp_format_error(reply, sizeof(reply), "ERR price out of range");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int n = resp_format_integer(reply, sizeof(reply), qty);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "BOOK")) {
    resp_node_t *price_arg = array_nth(cmd, 1);
    if (price_arg == NULL || price_arg->type != RESP_TYPE_BULK) {
      int n =
          resp_format_error(reply, sizeof(reply),
                            "ERR wrong number of arguments for 'book' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int price = 0;
    if (parse_int(price_arg->bulk, price_arg->bulk_len, &price) != 0) {
      int n = resp_format_error(reply, sizeof(reply), "ERR invalid price");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int64_t bid = 0, ask = 0;
    if (zedis_book_level(&server->book, price, &bid, &ask) != 0) {
      int n = resp_format_error(reply, sizeof(reply), "ERR price out of range");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int n = snprintf(reply, sizeof(reply),
                     "*2\r\n:%" PRId64 "\r\n:%" PRId64 "\r\n", bid, ask);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "DEL")) {
    resp_node_t *key = array_nth(cmd, 1);
    if (key == NULL || key->type != RESP_TYPE_BULK) {
      int n =
          resp_format_error(reply, sizeof(reply),
                            "ERR wrong number of arguments for 'del' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int deleted =
        (zedis_ht_del(&server->kv, key->bulk, key->bulk_len) == 0) ? 1 : 0;
    int n = resp_format_integer(reply, sizeof(reply), deleted);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "ZADD")) {
    resp_node_t *key = array_nth(cmd, 1);
    resp_node_t *score_arg = array_nth(cmd, 2);
    resp_node_t *member = array_nth(cmd, 3);
    if (key == NULL || score_arg == NULL || member == NULL ||
        key->type != RESP_TYPE_BULK || score_arg->type != RESP_TYPE_BULK ||
        member->type != RESP_TYPE_BULK) {
      int n =
          resp_format_error(reply, sizeof(reply),
                            "ERR wrong number of arguments for 'zadd' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int64_t score = 0;
    if (parse_i64(score_arg->bulk, score_arg->bulk_len, &score) != 0) {
      int n = resp_format_error(reply, sizeof(reply),
                                "ERR value is not a valid float");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int64_t prev = 0;
    int existed =
        (zedis_zset_reg_score(&server->zsets, key->bulk, key->bulk_len,
                              member->bulk, member->bulk_len, &prev) == 0);
    if (zedis_zset_reg_add(&server->zsets, key->bulk, key->bulk_len, score,
                           member->bulk, member->bulk_len) != 0) {
      int n = resp_format_error(reply, sizeof(reply),
                                "ERR zset full or member too large");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int n = resp_format_integer(reply, sizeof(reply), existed ? 0 : 1);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "ZSCORE")) {
    resp_node_t *key = array_nth(cmd, 1);
    resp_node_t *member = array_nth(cmd, 2);
    if (key == NULL || member == NULL || key->type != RESP_TYPE_BULK ||
        member->type != RESP_TYPE_BULK) {
      int n = resp_format_error(
          reply, sizeof(reply),
          "ERR wrong number of arguments for 'zscore' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int64_t score = 0;
    if (zedis_zset_reg_score(&server->zsets, key->bulk, key->bulk_len,
                             member->bulk, member->bulk_len, &score) != 0) {
      int n = resp_format_null(reply, sizeof(reply));
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    char score_buf[64];
    int slen = snprintf(score_buf, sizeof(score_buf), "%.17g", (double)score);
    if (slen > 0) {
      int n = resp_format_bulk(reply, sizeof(reply), score_buf, (size_t)slen);
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    }
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "ZRANGE")) {
    resp_node_t *key = array_nth(cmd, 1);
    resp_node_t *start_arg = array_nth(cmd, 2);
    resp_node_t *stop_arg = array_nth(cmd, 3);
    if (key == NULL || start_arg == NULL || stop_arg == NULL ||
        key->type != RESP_TYPE_BULK || start_arg->type != RESP_TYPE_BULK ||
        stop_arg->type != RESP_TYPE_BULK) {
      int n = resp_format_error(
          reply, sizeof(reply),
          "ERR wrong number of arguments for 'zrange' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int64_t start = 0, stop = 0;
    if (parse_i64(start_arg->bulk, start_arg->bulk_len, &start) != 0 ||
        parse_i64(stop_arg->bulk, stop_arg->bulk_len, &stop) != 0) {
      int n = resp_format_error(reply, sizeof(reply),
                                "ERR value is not an integer or out of range");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    char zreply[8192];
    int zlen = 0;
    if (zedis_zset_reg_range(&server->zsets, key->bulk, key->bulk_len, start,
                             stop, zreply, sizeof(zreply), &zlen) != 0) {
      int n = resp_format_error(reply, sizeof(reply), "ERR zrange failed");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    conn_queue_write(conn, zreply, (size_t)zlen);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "LPUSH")) {
    resp_node_t *key = array_nth(cmd, 1);
    resp_node_t *val = array_nth(cmd, 2);
    if (key == NULL || val == NULL || key->type != RESP_TYPE_BULK ||
        val->type != RESP_TYPE_BULK) {
      int n = resp_format_error(
          reply, sizeof(reply),
          "ERR wrong number of arguments for 'lpush' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    if (zedis_list_reg_lpush(&server->lists, &server->aol, key->bulk,
                             key->bulk_len, val->bulk, val->bulk_len) != 0) {
      int n = resp_format_error(reply, sizeof(reply),
                                "ERR list full or append log full");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    uint32_t len = 0;
    zedis_list_reg_llen(&server->lists, key->bulk, key->bulk_len, &len);
    int n = resp_format_integer(reply, sizeof(reply), (int64_t)len);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "LRANGE")) {
    resp_node_t *key = array_nth(cmd, 1);
    resp_node_t *start_arg = array_nth(cmd, 2);
    resp_node_t *stop_arg = array_nth(cmd, 3);
    if (key == NULL || start_arg == NULL || stop_arg == NULL ||
        key->type != RESP_TYPE_BULK || start_arg->type != RESP_TYPE_BULK ||
        stop_arg->type != RESP_TYPE_BULK) {
      int n = resp_format_error(
          reply, sizeof(reply),
          "ERR wrong number of arguments for 'lrange' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    int64_t start = 0, stop = 0;
    if (parse_i64(start_arg->bulk, start_arg->bulk_len, &start) != 0 ||
        parse_i64(stop_arg->bulk, stop_arg->bulk_len, &stop) != 0) {
      int n = resp_format_error(reply, sizeof(reply),
                                "ERR value is not an integer or out of range");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    char lreply[8192];
    int llen = 0;
    if (zedis_list_reg_lrange(&server->lists, &server->aol, key->bulk,
                              key->bulk_len, start, stop, lreply,
                              sizeof(lreply), &llen) != 0) {
      int n = resp_format_error(reply, sizeof(reply), "ERR lrange failed");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    conn_queue_write(conn, lreply, (size_t)llen);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "LLEN")) {
    resp_node_t *key = array_nth(cmd, 1);
    if (key == NULL || key->type != RESP_TYPE_BULK) {
      int n =
          resp_format_error(reply, sizeof(reply),
                            "ERR wrong number of arguments for 'llen' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }
    uint32_t len = 0;
    zedis_list_reg_llen(&server->lists, key->bulk, key->bulk_len, &len);
    int n = resp_format_integer(reply, sizeof(reply), (int64_t)len);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "PUBLISH")) {
    resp_node_t *channel = array_nth(cmd, 1);
    resp_node_t *message = array_nth(cmd, 2);
    if (channel == NULL || message == NULL || channel->type != RESP_TYPE_BULK ||
        message->type != RESP_TYPE_BULK) {
      int n = resp_format_error(
          reply, sizeof(reply),
          "ERR wrong number of arguments for 'publish' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }

    char ch[64];
    size_t ch_len =
        channel->bulk_len < sizeof(ch) - 1 ? channel->bulk_len : sizeof(ch) - 1;
    memcpy(ch, channel->bulk, ch_len);
    ch[ch_len] = '\0';

    if (zedis_ring_publish(&server->ring, ch, message->bulk,
                           message->bulk_len) != 0) {
      int n = resp_format_error(reply, sizeof(reply), "ERR ring buffer full");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }

    size_t receivers = 0;
    for (conn_t *c = server->conns_head; c != NULL; c = c->next) {
      if (c->subscribed && str_case_equal(c->subscribe_channel,
                                          strlen(c->subscribe_channel), ch)) {
        receivers++;
      }
    }

    server_broadcast_channel(server, ch, message->bulk, message->bulk_len);

    int n = resp_format_integer(reply, sizeof(reply), (int64_t)receivers);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "SUBSCRIBE")) {
    resp_node_t *channel = array_nth(cmd, 1);
    if (channel == NULL || channel->type != RESP_TYPE_BULK) {
      int n = resp_format_error(
          reply, sizeof(reply),
          "ERR wrong number of arguments for 'subscribe' command");
      if (n > 0) conn_queue_write(conn, reply, (size_t)n);
      return 0;
    }

    /* Copy into conn — this must survive past the current parse buffer. */
    size_t ch_len = channel->bulk_len < sizeof(conn->subscribe_channel) - 1
                        ? channel->bulk_len
                        : sizeof(conn->subscribe_channel) - 1;
    memcpy(conn->subscribe_channel, channel->bulk, ch_len);
    conn->subscribe_channel[ch_len] = '\0';
    conn->subscribed = 1;

    int n = snprintf(reply, sizeof(reply),
                     "*3\r\n$9\r\nsubscribe\r\n$%zu\r\n%.*s\r\n:1\r\n", ch_len,
                     (int)ch_len, conn->subscribe_channel);
    if (n > 0) conn_queue_write(conn, reply, (size_t)n);
    return 0;
  }

  if (str_case_equal(command->bulk, command->bulk_len, "QUIT")) {
    conn_destroy(conn);
    return 1;
  }

  int n = snprintf(reply, sizeof(reply), "-ERR unknown command '%.*s'\r\n",
                   (int)command->bulk_len, command->bulk);
  if (n > 0) conn_queue_write(conn, reply, (size_t)n);
  return 0;
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